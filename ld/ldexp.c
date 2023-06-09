/* This module handles expression trees.
   Copyright 1991, 1992, 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2000,
   2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009
   Free Software Foundation, Inc.
   Written by Steve Chamberlain of Cygnus Support <sac@cygnus.com>.

   This file is part of the GNU Binutils.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */


/* This module is in charge of working out the contents of expressions.

   It has to keep track of the relative/absness of a symbol etc. This
   is done by keeping all values in a struct (an etree_value_type)
   which contains a value, a section to which it is relative and a
   valid bit.  */

#include "sysdep.h"
#include "bfd.h"
#include "bfdlink.h"

#include "ld.h"
#include "ldmain.h"
#include "ldmisc.h"
#include "ldexp.h"
#include "ldlex.h"
#include <ldgram.h>
#include "ldlang.h"
#include "libiberty.h"
#include "safe-ctype.h"

static void exp_fold_tree_1 (etree_type *);
static void exp_fold_tree_no_dot (etree_type *);
static bfd_vma align_n (bfd_vma, bfd_vma);

segment_type *segments;

struct ldexp_control expld;

/* Print the string representation of the given token.  Surround it
   with spaces if INFIX_P is TRUE.  */

static const struct
{
    token_code_type code;
    char * name;
}
table[] =
{
    { INT, "int" },
    { NAME, "NAME" },
    { PLUSEQ, "+=" },
    { MINUSEQ, "-=" },
    { MULTEQ, "*=" },
    { DIVEQ, "/=" },
    { LSHIFTEQ, "<<=" },
    { RSHIFTEQ, ">>=" },
    { ANDEQ, "&=" },
    { OREQ, "|=" },
    { OROR, "||" },
    { ANDAND, "&&" },
    { EQ, "==" },
    { NE, "!=" },
    { LE, "<=" },
    { GE, ">=" },
    { LSHIFT, "<<" },
    { RSHIFT, ">>" },
    { ALIGN_K, "ALIGN" },
    { BLOCK, "BLOCK" },
    { QUAD, "QUAD" },
    { SQUAD, "SQUAD" },
    { LONG, "LONG" },
    { SHORT, "SHORT" },
    { BYTE, "BYTE" },
    { SECTIONS, "SECTIONS" },
    { SIZEOF_HEADERS, "SIZEOF_HEADERS" },
    { MEMORY, "MEMORY" },
    { DEFINED, "DEFINED" },
    { TARGET_K, "TARGET" },
    { SEARCH_DIR, "SEARCH_DIR" },
    { MAP, "MAP" },
    { ENTRY, "ENTRY" },
    { NEXT, "NEXT" },
    { ALIGNOF, "ALIGNOF" },
    { SIZEOF, "SIZEOF" },
    { ADDR, "ADDR" },
    { LOADADDR, "LOADADDR" },
    { CONSTANT, "CONSTANT" },
    { ABSOLUTE, "ABSOLUTE" },
    { MAX_K, "MAX" },
    { MIN_K, "MIN" },
    { ASSERT_K, "ASSERT" },
    { REL, "relocatable" },
    { DATA_SEGMENT_ALIGN, "DATA_SEGMENT_ALIGN" },
    { DATA_SEGMENT_RELRO_END, "DATA_SEGMENT_RELRO_END" },
    { DATA_SEGMENT_END, "DATA_SEGMENT_END" },
    { ORIGIN, "ORIGIN" },
    { LENGTH, "LENGTH" },
    { SEGMENT_START, "SEGMENT_START" }
};

static void
exp_print_token (token_code_type code, int infix_p)
{
  unsigned int idx;

  for (idx = 0; idx < ARRAY_SIZE (table); idx++)
    if (table[idx].code == code)
      break;

  if (infix_p)
    fputc (' ', config.map_file);

  if (idx < ARRAY_SIZE (table))
    fputs (table[idx].name, config.map_file);
  else if (code < 127)
    fputc (code, config.map_file);
  else
    fprintf (config.map_file, "<code %d>", code);

  if (infix_p)
    fputc (' ', config.map_file);
}

static char*
exp_token_to_string (token_code_type code, int infix_p)
{
  unsigned int idx;
  const char *inf_space;
  char* buf;

  inf_space = (infix_p) ? " " : "";
  for (idx = 0; idx < ARRAY_SIZE (table); idx++)
    if (table[idx].code == code)
      break;


  if (idx < ARRAY_SIZE (table))
    {
      buf = xmalloc (strlen (table[idx].name) +
                     (strlen (inf_space) * 2) +
                     1);
      sprintf (buf, "%s%s%s", inf_space, table[idx].name, inf_space);
    }
  else if (code < 127)
    {

      buf = xmalloc (strlen (inf_space) * 2 + 2);
      sprintf (buf, "%s%c%s", inf_space, code, inf_space);
    }
  else
    {
      buf = xmalloc (strlen ("<code −2147483648>") +
                     (strlen (inf_space) * 2) +
                     1);
      sprintf (buf, "%s<code %d>%s", inf_space, code, inf_space);
    }

  return buf;
}

static void
make_abs (void)
{
  expld.result.value += expld.result.section->vma;
  expld.result.section = bfd_abs_section_ptr;
}

static void
new_abs (bfd_vma value)
{
  expld.result.valid_p = TRUE;
  expld.result.section = bfd_abs_section_ptr;
  expld.result.value = value;
  expld.result.str = NULL;
}

etree_type *
exp_intop (bfd_vma value)
{
  etree_type *new_e = (etree_type *) stat_alloc (sizeof (new_e->value));
  new_e->type.node_code = INT;
  new_e->type.lineno = lineno;
  new_e->value.value = value;
  new_e->value.str = NULL;
  new_e->type.node_class = etree_value;
  return new_e;
}

etree_type *
exp_bigintop (bfd_vma value, char *str)
{
  etree_type *new_e = (etree_type *) stat_alloc (sizeof (new_e->value));
  new_e->type.node_code = INT;
  new_e->type.lineno = lineno;
  new_e->value.value = value;
  new_e->value.str = str;
  new_e->type.node_class = etree_value;
  return new_e;
}

/* Build an expression representing an unnamed relocatable value.  */

etree_type *
exp_relop (asection *section, bfd_vma value)
{
  etree_type *new_e = (etree_type *) stat_alloc (sizeof (new_e->rel));
  new_e->type.node_code = REL;
  new_e->type.lineno = lineno;
  new_e->type.node_class = etree_rel;
  new_e->rel.section = section;
  new_e->rel.value = value;
  return new_e;
}

static void
new_rel (bfd_vma value, char *str, asection *section)
{
  expld.result.valid_p = TRUE;
  expld.result.value = value;
  expld.result.str = str;
  expld.result.section = section;
}

static void
new_rel_from_abs (bfd_vma value)
{
  expld.result.valid_p = TRUE;
  expld.result.value = value - expld.section->vma;
  expld.result.str = NULL;
  expld.result.section = expld.section;
}

static void
fold_unary (etree_type *tree)
{
  exp_fold_tree_1 (tree->unary.child);
  if (expld.result.valid_p)
    {
      switch (tree->type.node_code)
	{
	case ALIGN_K:
	  if (expld.phase != lang_first_phase_enum)
	    new_rel_from_abs (align_n (expld.dot, expld.result.value));
	  else
	    expld.result.valid_p = FALSE;
	  break;

	case ABSOLUTE:
	  make_abs ();
	  break;

	case '~':
	  make_abs ();
	  expld.result.value = ~expld.result.value;
	  break;

	case '!':
	  make_abs ();
	  expld.result.value = !expld.result.value;
	  break;

	case '-':
	  make_abs ();
	  expld.result.value = -expld.result.value;
	  break;

	case NEXT:
	  /* Return next place aligned to value.  */
	  if (expld.phase != lang_first_phase_enum)
	    {
	      make_abs ();
	      expld.result.value = align_n (expld.dot, expld.result.value);
	    }
	  else
	    expld.result.valid_p = FALSE;
	  break;

	case DATA_SEGMENT_END:
	  if (expld.phase != lang_first_phase_enum
	      && expld.section == bfd_abs_section_ptr
	      && (expld.dataseg.phase == exp_dataseg_align_seen
		  || expld.dataseg.phase == exp_dataseg_relro_seen
		  || expld.dataseg.phase == exp_dataseg_adjust
		  || expld.dataseg.phase == exp_dataseg_relro_adjust
		  || expld.phase == lang_final_phase_enum))
	    {
	      if (expld.dataseg.phase == exp_dataseg_align_seen
		  || expld.dataseg.phase == exp_dataseg_relro_seen)
		{
		  expld.dataseg.phase = exp_dataseg_end_seen;
		  expld.dataseg.end = expld.result.value;
		}
	    }
	  else
	    expld.result.valid_p = FALSE;
	  break;

	default:
	  FAIL ();
	  break;
	}
    }
}

static void
fold_binary (etree_type *tree)
{
  etree_value_type lhs;
  exp_fold_tree_1 (tree->binary.lhs);

  /* The SEGMENT_START operator is special because its first
     operand is a string, not the name of a symbol.  Note that the
     operands have been swapped, so binary.lhs is second (default)
     operand, binary.rhs is first operand.  */
  if (expld.result.valid_p && tree->type.node_code == SEGMENT_START)
    {
      const char *segment_name;
      segment_type *seg;
      /* Check to see if the user has overridden the default
	 value.  */
      segment_name = tree->binary.rhs->name.name;
      for (seg = segments; seg; seg = seg->next) 
	if (strcmp (seg->name, segment_name) == 0)
	  {
	    seg->used = TRUE;
	    expld.result.value = seg->value;
	    expld.result.str = NULL;
	    expld.result.section = expld.section;
	    break;
	  }
      return;
    }

  lhs = expld.result;
  exp_fold_tree_1 (tree->binary.rhs);
  expld.result.valid_p &= lhs.valid_p;

  if (expld.result.valid_p)
    {
      /* If the values are from different sections, or this is an
	 absolute expression, make both the source arguments
	 absolute.  However, adding or subtracting an absolute
	 value from a relative value is meaningful, and is an
	 exception.  */
      if (expld.section != bfd_abs_section_ptr
	  && lhs.section == bfd_abs_section_ptr
	  && tree->type.node_code == '+')
	{
	  /* Keep the section of the rhs term.  */
	  expld.result.value = lhs.value + expld.result.value;
	  return;
	}
      else if (expld.section != bfd_abs_section_ptr
	       && expld.result.section == bfd_abs_section_ptr
	       && (tree->type.node_code == '+'
		   || tree->type.node_code == '-'))
	{
	  /* Keep the section of the lhs term.  */
	  expld.result.section = lhs.section;
	}
      else if (expld.result.section != lhs.section
	       || expld.section == bfd_abs_section_ptr)
	{
	  make_abs ();
	  lhs.value += lhs.section->vma;
	}

      switch (tree->type.node_code)
	{
	case '%':
	  if (expld.result.value != 0)
	    expld.result.value = ((bfd_signed_vma) lhs.value
				  % (bfd_signed_vma) expld.result.value);
	  else if (expld.phase != lang_mark_phase_enum)
            {
              lineno = tree->type.lineno;
              einfo (_("%F%S: error: %% by zero\n"));
            }
	  break;

	case '/':
	  if (expld.result.value != 0)
	    expld.result.value = ((bfd_signed_vma) lhs.value
				  / (bfd_signed_vma) expld.result.value);
	  else if (expld.phase != lang_mark_phase_enum)
            {
              lineno = tree->type.lineno;
              einfo (_("%F%S: error: / by zero\n"));
            }
	  break;

#define BOP(x, y) \
	    case x:							\
	      expld.result.value = lhs.value y expld.result.value;	\
	      break;

	  BOP ('+', +);
	  BOP ('*', *);
	  BOP ('-', -);
	  BOP (LSHIFT, <<);
	  BOP (RSHIFT, >>);
	  BOP (EQ, ==);
	  BOP (NE, !=);
	  BOP ('<', <);
	  BOP ('>', >);
	  BOP (LE, <=);
	  BOP (GE, >=);
	  BOP ('&', &);
	  BOP ('^', ^);
	  BOP ('|', |);
	  BOP (ANDAND, &&);
	  BOP (OROR, ||);

	case MAX_K:
	  if (lhs.value > expld.result.value)
	    expld.result.value = lhs.value;
	  break;

	case MIN_K:
	  if (lhs.value < expld.result.value)
	    expld.result.value = lhs.value;
	  break;

	case ALIGN_K:
	  expld.result.value = align_n (lhs.value, expld.result.value);
	  break;

	case DATA_SEGMENT_ALIGN:
	  expld.dataseg.relro = exp_dataseg_relro_start;
	  if (expld.phase != lang_first_phase_enum
	      && expld.section == bfd_abs_section_ptr
	      && (expld.dataseg.phase == exp_dataseg_none
		  || expld.dataseg.phase == exp_dataseg_adjust
		  || expld.dataseg.phase == exp_dataseg_relro_adjust
		  || expld.phase == lang_final_phase_enum))
	    {
	      bfd_vma maxpage = lhs.value;
	      bfd_vma commonpage = expld.result.value;

	      expld.result.value = align_n (expld.dot, maxpage);
	      if (expld.dataseg.phase == exp_dataseg_relro_adjust)
		expld.result.value = expld.dataseg.base;
	      else if (expld.dataseg.phase != exp_dataseg_adjust)
		{
		  expld.result.value += expld.dot & (maxpage - 1);
		  if (expld.phase == lang_allocating_phase_enum)
		    {
		      expld.dataseg.phase = exp_dataseg_align_seen;
		      expld.dataseg.min_base = expld.dot;
		      expld.dataseg.base = expld.result.value;
		      expld.dataseg.pagesize = commonpage;
		      expld.dataseg.maxpagesize = maxpage;
		      expld.dataseg.relro_end = 0;
		    }
		}
	      else if (commonpage < maxpage)
		expld.result.value += ((expld.dot + commonpage - 1)
				       & (maxpage - commonpage));
	    }
	  else
	    expld.result.valid_p = FALSE;
	  break;

	case DATA_SEGMENT_RELRO_END:
	  expld.dataseg.relro = exp_dataseg_relro_end;
	  if (expld.phase != lang_first_phase_enum
	      && (expld.dataseg.phase == exp_dataseg_align_seen
		  || expld.dataseg.phase == exp_dataseg_adjust
		  || expld.dataseg.phase == exp_dataseg_relro_adjust
		  || expld.phase == lang_final_phase_enum))
	    {
	      if (expld.dataseg.phase == exp_dataseg_align_seen
		  || expld.dataseg.phase == exp_dataseg_relro_adjust)
		expld.dataseg.relro_end = lhs.value + expld.result.value;

	      if (expld.dataseg.phase == exp_dataseg_relro_adjust
		  && (expld.dataseg.relro_end
		      & (expld.dataseg.pagesize - 1)))
		{
		  expld.dataseg.relro_end += expld.dataseg.pagesize - 1;
		  expld.dataseg.relro_end &= ~(expld.dataseg.pagesize - 1);
		  expld.result.value = (expld.dataseg.relro_end
					- expld.result.value);
		}
	      else
		expld.result.value = lhs.value;

	      if (expld.dataseg.phase == exp_dataseg_align_seen)
		expld.dataseg.phase = exp_dataseg_relro_seen;
	    }
	  else
	    expld.result.valid_p = FALSE;
	  break;

	default:
	  FAIL ();
	}
    }
}

static void
fold_trinary (etree_type *tree)
{
  exp_fold_tree_1 (tree->trinary.cond);
  if (expld.result.valid_p)
    exp_fold_tree_1 (expld.result.value
		     ? tree->trinary.lhs
		     : tree->trinary.rhs);
}

static void
fold_name (etree_type *tree)
{
  memset (&expld.result, 0, sizeof (expld.result));

  switch (tree->type.node_code)
    {
    case SIZEOF_HEADERS:
      if (expld.phase != lang_first_phase_enum)
	{
	  bfd_vma hdr_size = 0;
	  /* Don't find the real header size if only marking sections;
	     The bfd function may cache incorrect data.  */
	  if (expld.phase != lang_mark_phase_enum)
	    hdr_size = bfd_sizeof_headers (link_info.output_bfd, &link_info);
	  new_abs (hdr_size);
	}
      break;

    case DEFINED:
      if (expld.phase == lang_first_phase_enum)
	lang_track_definedness (tree->name.name);
      else
	{
	  struct bfd_link_hash_entry *h;
	  int def_iteration
	    = lang_symbol_definition_iteration (tree->name.name);

	  h = bfd_wrapped_link_hash_lookup (link_info.output_bfd,
					    &link_info,
					    tree->name.name,
					    FALSE, FALSE, TRUE);
	  expld.result.value = (h != NULL
				&& (h->type == bfd_link_hash_defined
				    || h->type == bfd_link_hash_defweak
				    || h->type == bfd_link_hash_common)
				&& (def_iteration == lang_statement_iteration
				    || def_iteration == -1));
	  expld.result.section = expld.section;
	  expld.result.valid_p = TRUE;
	}
      break;

    case NAME:
      if (expld.phase == lang_first_phase_enum)
	;
      else if (tree->name.name[0] == '.' && tree->name.name[1] == 0)
	new_rel_from_abs (expld.dot);
      else
	{
	  struct bfd_link_hash_entry *h;

	  h = bfd_wrapped_link_hash_lookup (link_info.output_bfd,
					    &link_info,
					    tree->name.name,
					    TRUE, FALSE, TRUE);
	  if (!h)
	    einfo (_("%P%F: error: bfd_link_hash_lookup failed: %E\n"));
	  else if (h->type == bfd_link_hash_defined
		   || h->type == bfd_link_hash_defweak)
	    {
	      if (bfd_is_abs_section (h->u.def.section))
		new_abs (h->u.def.value);
	      else
		{
		  asection *output_section;

		  output_section = h->u.def.section->output_section;
		  if (output_section == NULL)
		    {
		      if (expld.phase != lang_mark_phase_enum)
                        {
                          lineno = tree->type.lineno;
                          einfo (_("%X%S: error: unresolvable symbol `%s'"
                                   " referenced in expression\n"),
                                 tree->name.name);
                        }
		    }
		  else
		    new_rel (h->u.def.value + h->u.def.section->output_offset,
			     NULL, output_section);
		}
	    }
	  else if (expld.phase == lang_final_phase_enum
		   || expld.assigning_to_dot)
            {
              lineno = tree->type.lineno;
              einfo (_("%F%S: error: undefined symbol `%s' referenced in expression\n"),
                     tree->name.name);
            }
	  else if (h->type == bfd_link_hash_new)
	    {
	      h->type = bfd_link_hash_undefined;
	      h->u.undef.abfd = NULL;
	      if (h->u.undef.next == NULL && h != link_info.hash->undefs_tail)
		bfd_link_add_undef (link_info.hash, h);
	    }
	}
      break;

    case ADDR:
      if (expld.phase != lang_first_phase_enum)
	{
	  lang_output_section_statement_type *os;

	  os = lang_output_section_find (tree->name.name);
	  if (os == NULL)
	    {
	      if (expld.phase == lang_final_phase_enum)
                {
                  lineno = tree->type.lineno;
                  einfo (_("%F%S: error: undefined section `%s' referenced in expression\n"),
                         tree->name.name);
                }
	    }
	  else if (os->processed_vma)
	    new_rel (0, NULL, os->bfd_section);
	}
      break;

    case LOADADDR:
      if (expld.phase != lang_first_phase_enum)
	{
	  lang_output_section_statement_type *os;

	  os = lang_output_section_find (tree->name.name);
	  if (os == NULL)
	    {
	      if (expld.phase == lang_final_phase_enum)
                {
                  lineno = tree->type.lineno;
                  einfo (_("%F%S: error: undefined section `%s' referenced in expression\n"),
                         tree->name.name);
                }
	    }
	  else if (os->processed_lma)
	    {
	      if (os->load_base == NULL)
		new_abs (os->bfd_section->lma);
	      else
		{
		  exp_fold_tree_1 (os->load_base);
		  if (expld.result.valid_p)
		    make_abs ();
		}
	    }
	}
      break;

    case SIZEOF:
    case ALIGNOF:
      if (expld.phase != lang_first_phase_enum)
	{
	  lang_output_section_statement_type *os;

	  os = lang_output_section_find (tree->name.name);
	  if (os == NULL)
	    {
	      if (expld.phase == lang_final_phase_enum)
                {
                  lineno = tree->type.lineno;
                  einfo (_("%F%S: error: undefined section `%s' referenced in expression\n"),
                         tree->name.name);
                }
	      new_abs (0);
	    }
	  else if (os->processed_vma)
	    {
	      bfd_vma val;

	      if (tree->type.node_code == SIZEOF)
		val = (os->bfd_section->size
		       / bfd_octets_per_byte (link_info.output_bfd));
	      else
		val = (bfd_vma)1 << os->bfd_section->alignment_power;
	      
	      new_abs (val);
	    }
	}
      break;

    case LENGTH:
      {
        lang_memory_region_type *mem;
        
        mem = lang_memory_region_lookup (tree->name.name, FALSE);  
        if (mem != NULL) 
          new_abs (mem->length);
        else
          {
            lineno = tree->type.lineno;
            einfo (_("%F%S: error: undefined MEMORY region `%s'"
                     " referenced in expression\n"), tree->name.name);
          }
      }
      break;

    case ORIGIN:
      {
        lang_memory_region_type *mem;
        
        mem = lang_memory_region_lookup (tree->name.name, FALSE);  
        if (mem != NULL) 
          new_abs (mem->origin);
        else
          {
            lineno = tree->type.lineno;
            einfo (_("%F%S: error: undefined MEMORY region `%s'"
                     " referenced in expression\n"), tree->name.name);
          }
      }
      break;

    case CONSTANT:
      if (strcmp (tree->name.name, "MAXPAGESIZE") == 0)
	new_abs (config.maxpagesize);
      else if (strcmp (tree->name.name, "COMMONPAGESIZE") == 0)
	new_abs (config.commonpagesize);
      else
        {
          lineno = tree->type.lineno;
          einfo (_("%F%S: error: unknown constant `%s' referenced in expression\n"),
                 tree->name.name);
        }
      break;

    default:
      FAIL ();
      break;
    }
}

static void
exp_fold_tree_1 (etree_type *tree)
{
  if (tree == NULL)
    {
      memset (&expld.result, 0, sizeof (expld.result));
      return;
    }

  switch (tree->type.node_class)
    {
    case etree_value:
      new_rel (tree->value.value, tree->value.str, expld.section);
      break;

    case etree_rel:
      if (expld.phase != lang_first_phase_enum)
	{
	  asection *output_section = tree->rel.section->output_section;
	  new_rel (tree->rel.value + tree->rel.section->output_offset,
		   NULL, output_section);
	}
      else
	memset (&expld.result, 0, sizeof (expld.result));
      break;

    case etree_assert:
      exp_fold_tree_1 (tree->assert_s.child);
      if (expld.phase == lang_final_phase_enum && !expld.result.value)
	einfo (_("%X%P: error: %s\n"), tree->assert_s.message);
      break;

    case etree_unary:
      fold_unary (tree);
      break;

    case etree_binary:
      fold_binary (tree);
      break;

    case etree_trinary:
      fold_trinary (tree);
      break;

    case etree_assign:
    case etree_provide:
    case etree_provided:
      if (tree->assign.dst[0] == '.' && tree->assign.dst[1] == 0)
	{
	  /* Assignment to dot can only be done during allocation.  */
	  if (tree->type.node_class != etree_assign)
            {
              lineno = tree->type.lineno;
              einfo (_("%F%S: error: can not PROVIDE assignment to location counter\n"));
            }
	  if (expld.phase == lang_mark_phase_enum
	      || expld.phase == lang_allocating_phase_enum
	      || (expld.phase == lang_final_phase_enum
		  && expld.section == bfd_abs_section_ptr))
	    {
	      /* Notify the folder that this is an assignment to dot.  */
	      expld.assigning_to_dot = TRUE;
	      exp_fold_tree_1 (tree->assign.src);
	      expld.assigning_to_dot = FALSE;

	      if (!expld.result.valid_p)
		{
		  if (expld.phase != lang_mark_phase_enum)
                    {
                      lineno = tree->type.lineno;
                      einfo (_("%F%S: error: invalid assignment to location counter\n"));
                    }
		}
	      else if (expld.dotp == NULL)
                {
                  lineno = tree->type.lineno;
                  einfo (_("%F%S: error: assignment to location counter"
                           " invalid outside of SECTION\n"));
                }
	      else
		{
		  bfd_vma nextdot;

		  nextdot = expld.result.value + expld.section->vma;
		  if (nextdot < expld.dot
		      && expld.section != bfd_abs_section_ptr)
                    {
                      lineno = tree->type.lineno;
                      einfo (_("%F%S: error: cannot move location counter backwards"
                             " in %A (from %V to %V)\n"), expld.section, expld.dot, nextdot);
                    }
		  else
		    {
		      expld.dot = nextdot;
		      *expld.dotp = nextdot;
		    }
		}
	    }
	  else
	    memset (&expld.result, 0, sizeof (expld.result));
	}
      else
	{
	  struct bfd_link_hash_entry *h = NULL;

	  if (tree->type.node_class == etree_provide)
	    {
	      h = bfd_link_hash_lookup (link_info.hash, tree->assign.dst,
					FALSE, FALSE, TRUE);
	      if (h == NULL
		  || (h->type != bfd_link_hash_new
		      && h->type != bfd_link_hash_undefined
		      && h->type != bfd_link_hash_common))
		{
		  /* Do nothing.  The symbol was never referenced, or was
		     defined by some object.  */
		  break;
		}
	    }

	  exp_fold_tree_1 (tree->assign.src);
	  if (expld.result.valid_p)
	    {
	      if (h == NULL)
		{
		  h = bfd_link_hash_lookup (link_info.hash, tree->assign.dst,
					    TRUE, FALSE, TRUE);
		  if (h == NULL)
		    einfo (_("%P%F: error:%s: hash creation failed\n"),
			   tree->assign.dst);
		}

	      /* FIXME: Should we worry if the symbol is already
		 defined?  */
	      lang_update_definedness (tree->assign.dst, h);
	      h->type = bfd_link_hash_defined;
	      h->u.def.value = expld.result.value;
	      h->u.def.section = expld.result.section;
	      if (tree->type.node_class == etree_provide)
		tree->type.node_class = etree_provided;
	    }
	}
      break;

    case etree_name:
      fold_name (tree);
      break;

    default:
      FAIL ();
      memset (&expld.result, 0, sizeof (expld.result));
      break;
    }
}

void
exp_fold_tree (etree_type *tree, asection *current_section, bfd_vma *dotp)
{
  expld.dot = *dotp;
  expld.dotp = dotp;
  expld.section = current_section;
  exp_fold_tree_1 (tree);
}

static void
exp_fold_tree_no_dot (etree_type *tree)
{
  expld.dot = 0;
  expld.dotp = NULL;
  expld.section = bfd_abs_section_ptr;
  exp_fold_tree_1 (tree);
}

etree_type *
exp_binop (int code, etree_type *lhs, etree_type *rhs)
{
  etree_type value, *new_e;

  value.type.node_code = code;
  value.type.lineno = lhs->type.lineno;
  value.binary.lhs = lhs;
  value.binary.rhs = rhs;
  value.type.node_class = etree_binary;
  exp_fold_tree_no_dot (&value);
  if (expld.result.valid_p)
    return exp_intop (expld.result.value);

  new_e = (etree_type *) stat_alloc (sizeof (new_e->binary));
  memcpy (new_e, &value, sizeof (new_e->binary));
  return new_e;
}

etree_type *
exp_trinop (int code, etree_type *cond, etree_type *lhs, etree_type *rhs)
{
  etree_type value, *new_e;

  value.type.node_code = code;
  value.type.lineno = lhs->type.lineno;
  value.trinary.lhs = lhs;
  value.trinary.cond = cond;
  value.trinary.rhs = rhs;
  value.type.node_class = etree_trinary;
  exp_fold_tree_no_dot (&value);
  if (expld.result.valid_p)
    return exp_intop (expld.result.value);

  new_e = (etree_type *) stat_alloc (sizeof (new_e->trinary));
  memcpy (new_e, &value, sizeof (new_e->trinary));
  return new_e;
}

etree_type *
exp_unop (int code, etree_type *child)
{
  etree_type value, *new_e;

  value.unary.type.node_code = code;
  value.unary.type.lineno = child->type.lineno;
  value.unary.child = child;
  value.unary.type.node_class = etree_unary;
  exp_fold_tree_no_dot (&value);
  if (expld.result.valid_p)
    return exp_intop (expld.result.value);

  new_e = (etree_type *) stat_alloc (sizeof (new_e->unary));
  memcpy (new_e, &value, sizeof (new_e->unary));
  return new_e;
}

etree_type *
exp_nameop (int code, const char *name)
{
  etree_type value, *new_e;

  value.name.type.node_code = code;
  value.name.type.lineno = lineno;
  value.name.name = name;
  value.name.type.node_class = etree_name;

  exp_fold_tree_no_dot (&value);
  if (expld.result.valid_p)
    return exp_intop (expld.result.value);

  new_e = (etree_type *) stat_alloc (sizeof (new_e->name));
  memcpy (new_e, &value, sizeof (new_e->name));
  return new_e;

}

etree_type *
exp_assop (int code, const char *dst, etree_type *src)
{
  etree_type *new_e;

  new_e = (etree_type *) stat_alloc (sizeof (new_e->assign));
  new_e->type.node_code = code;
  new_e->type.lineno = src->type.lineno;
  new_e->type.node_class = etree_assign;
  new_e->assign.src = src;
  new_e->assign.dst = dst;
  return new_e;
}

/* Handle PROVIDE.  */

etree_type *
exp_provide (const char *dst, etree_type *src, bfd_boolean hidden)
{
  etree_type *n;

  n = (etree_type *) stat_alloc (sizeof (n->assign));
  n->assign.type.node_code = '=';
  n->assign.type.lineno = src->type.lineno;
  n->assign.type.node_class = etree_provide;
  n->assign.src = src;
  n->assign.dst = dst;
  n->assign.hidden = hidden;
  return n;
}

/* Handle ASSERT.  */

etree_type *
exp_assert (etree_type *exp, const char *message)
{
  etree_type *n;

  n = (etree_type *) stat_alloc (sizeof (n->assert_s));
  n->assert_s.type.node_code = '!';
  n->assert_s.type.lineno = exp->type.lineno;
  n->assert_s.type.node_class = etree_assert;
  n->assert_s.child = exp;
  n->assert_s.message = message;
  return n;
}

void
exp_print_tree (etree_type *tree)
{
  if (config.map_file == NULL)
    config.map_file = stderr;

  if (tree == NULL)
    {
      minfo ("NULL TREE\n");
      return;
    }

  switch (tree->type.node_class)
    {
    case etree_value:
      minfo ("0x%v", tree->value.value);
      return;
    case etree_rel:
      if (tree->rel.section->owner != NULL)
	minfo ("%B:", tree->rel.section->owner);
      minfo ("%s+0x%v", tree->rel.section->name, tree->rel.value);
      return;
    case etree_assign:
      fprintf (config.map_file, "%s", tree->assign.dst);
      exp_print_token (tree->type.node_code, TRUE);
      exp_print_tree (tree->assign.src);
      break;
    case etree_provide:
    case etree_provided:
      fprintf (config.map_file, "PROVIDE (%s, ", tree->assign.dst);
      exp_print_tree (tree->assign.src);
      fprintf (config.map_file, ")");
      break;
    case etree_binary:
      fprintf (config.map_file, "(");
      exp_print_tree (tree->binary.lhs);
      exp_print_token (tree->type.node_code, TRUE);
      exp_print_tree (tree->binary.rhs);
      fprintf (config.map_file, ")");
      break;
    case etree_trinary:
      exp_print_tree (tree->trinary.cond);
      fprintf (config.map_file, "?");
      exp_print_tree (tree->trinary.lhs);
      fprintf (config.map_file, ":");
      exp_print_tree (tree->trinary.rhs);
      break;
    case etree_unary:
      exp_print_token (tree->unary.type.node_code, FALSE);
      if (tree->unary.child)
	{
	  fprintf (config.map_file, " (");
	  exp_print_tree (tree->unary.child);
	  fprintf (config.map_file, ")");
	}
      break;

    case etree_assert:
      fprintf (config.map_file, "ASSERT (");
      exp_print_tree (tree->assert_s.child);
      fprintf (config.map_file, ", %s)", tree->assert_s.message);
      break;

    case etree_name:
      if (tree->type.node_code == NAME)
	{
	  fprintf (config.map_file, "%s", tree->name.name);
	}
      else
	{
	  exp_print_token (tree->type.node_code, FALSE);
	  if (tree->name.name)
	    fprintf (config.map_file, " (%s)", tree->name.name);
	}
      break;
    default:
      FAIL ();
      break;
    }
}

char*
exp_tree_to_string (etree_type *tree)
{
  char *lhs = NULL;
  char *rhs = NULL;
  char *cond = NULL;
  char *op = NULL;
  char *buf = NULL;

  if (tree == NULL)
    {
      buf = xmalloc (strlen ("NULL_TREE") + 1);
      sprintf (buf, "NULL TREE");

      return buf;
    }

  switch (tree->type.node_class)
    {
    case etree_value:
      buf = xmalloc (strlen ("0xffffffff") + 1);
      sprintf (buf, "0x%08lx", tree->value.value);
      break;
    case etree_rel:
      buf = xmalloc (1);
      buf[0] = 0;
      break;
    case etree_assign:
      op = exp_token_to_string (tree->type.node_code, TRUE);
      rhs = exp_tree_to_string (tree->assign.src);

      buf = xmalloc (strlen (tree->assign.dst) +
                     strlen (op) +
                     strlen (rhs) +
                     1);

      sprintf (buf, "%s%s%s", tree->assign.dst, op, rhs);
      break;
    case etree_provide:
    case etree_provided:
      rhs = exp_tree_to_string (tree->assign.src);

      buf = xmalloc (strlen (tree->assign.dst) +
                     strlen (rhs) +
                     strlen ("PROVIDE (, )") +
                     1);

      sprintf (buf, "PROVIDE (%s, %s)", tree->assign.dst, rhs);
      break;
    case etree_binary:
      lhs = exp_tree_to_string (tree->binary.lhs);
      op = exp_token_to_string (tree->type.node_code, TRUE);
      rhs = exp_tree_to_string (tree->binary.rhs);

      buf = xmalloc (strlen (lhs) +
                     strlen (op) +
                     strlen (rhs) +
                     strlen ("()") +
                     1);

      sprintf (buf, "(%s%s%s)", lhs, op, rhs);
      break;
    case etree_trinary:
      cond = exp_tree_to_string (tree->trinary.cond);
      lhs = exp_tree_to_string (tree->trinary.lhs);
      rhs = exp_tree_to_string (tree->trinary.rhs);

      buf = xmalloc (strlen (cond) +
                     strlen (lhs) +
                     strlen (rhs) +
                     strlen (" ?  : ") +
                     1);

      sprintf (buf, "%s ? %s : %s", cond, lhs, rhs);
      break;
    case etree_unary:
      op = exp_token_to_string (tree->unary.type.node_code, FALSE);
      if (tree->unary.child)
        {
          rhs = exp_tree_to_string (tree->unary.child);

          buf = xmalloc (strlen (op) +
                         strlen (rhs) +
                         strlen ("()") +
                         1);

          sprintf (buf, "%s(%s)", op, rhs);
        }
      else
        {
          buf = xmalloc (strlen (op) + 1);
          sprintf (buf, "%s", op);
        }
      break;

    case etree_assert:
      rhs = exp_tree_to_string (tree->assert_s.child);

      buf = xmalloc (strlen (tree->assert_s.message) +
                     strlen (rhs) +
                     strlen ("ASSERT (, )") +
                     1);

      sprintf (buf, "ASSERT (%s, %s)", rhs, tree->assert_s.message);
      break;

    case etree_name:
      if (tree->type.node_code == NAME)
        {
          buf = xmalloc (strlen (tree->name.name) + 1);
          
          sprintf (buf, "%s", tree->name.name);
        }
      else
        {
          op = exp_token_to_string (tree->type.node_code, FALSE);

          if (tree->name.name)
            {
              buf = xmalloc (strlen (tree->name.name) +
                             strlen (op) +
                             strlen (" ()") +
                             1);
              sprintf (buf, "%s (%s)", op, tree->name.name);
            }
          else
            {
              buf = xmalloc (strlen (op) + 1);
              sprintf (buf, "%s", op);
            }
        }
      break;
    default:
      FAIL();
      break;
    }

  if (op)
    free (op);
  if (rhs)
    free (rhs);
  if (lhs)
    free (lhs);
  if (cond)
    free (cond);

  return buf;
}

bfd_vma
exp_get_vma (etree_type *tree, bfd_vma def, char *name)
{
  if (tree != NULL)
    {
      exp_fold_tree_no_dot (tree);
      if (expld.result.valid_p)
	return expld.result.value;
      else if (name != NULL && expld.phase != lang_mark_phase_enum)
        {
          lineno = tree->type.lineno;
          einfo (_("%F%S: error: nonconstant expression for %s\n"), name);
        }
    }
  return def;
}

int
exp_get_value_int (etree_type *tree, int def, char *name)
{
  return exp_get_vma (tree, def, name);
}

fill_type *
exp_get_fill (etree_type *tree, fill_type *def, char *name)
{
  fill_type *fill;
  size_t len;
  unsigned int val;

  if (tree == NULL)
    return def;

  exp_fold_tree_no_dot (tree);
  if (!expld.result.valid_p)
    {
      if (name != NULL && expld.phase != lang_mark_phase_enum)
        {
          lineno = tree->type.lineno;
          einfo (_("%F%S: error: nonconstant expression for %s\n"), name);
        }
      return def;
    }

  if (expld.result.str != NULL && (len = strlen (expld.result.str)) != 0)
    {
      unsigned char *dst;
      unsigned char *s;
      fill = (fill_type *) xmalloc ((len + 1) / 2 + sizeof (*fill) - 1);
      fill->size = (len + 1) / 2;
      dst = fill->data;
      s = (unsigned char *) expld.result.str;
      val = 0;
      do
	{
	  unsigned int digit;

	  digit = *s++ - '0';
	  if (digit > 9)
	    digit = (digit - 'A' + '0' + 10) & 0xf;
	  val <<= 4;
	  val += digit;
	  --len;
	  if ((len & 1) == 0)
	    {
	      *dst++ = val;
	      val = 0;
	    }
	}
      while (len != 0);
    }
  else
    {
      fill = (fill_type *) xmalloc (4 + sizeof (*fill) - 1);
      val = expld.result.value;
      fill->data[0] = (val >> 24) & 0xff;
      fill->data[1] = (val >> 16) & 0xff;
      fill->data[2] = (val >>  8) & 0xff;
      fill->data[3] = (val >>  0) & 0xff;
      fill->size = 4;
    }
  return fill;
}

bfd_vma
exp_get_abs_int (etree_type *tree, int def, char *name)
{
  if (tree != NULL)
    {
      exp_fold_tree_no_dot (tree);

      if (expld.result.valid_p)
	{
	  expld.result.value += expld.result.section->vma;
	  return expld.result.value;
	}
      else if (name != NULL && expld.phase != lang_mark_phase_enum)
	{
	  lineno = tree->type.lineno;
	  einfo (_("%F%S: error: nonconstant expression for %s\n"), name);
	}
    }
  return def;
}

static bfd_vma
align_n (bfd_vma value, bfd_vma align)
{
  int l;
  unsigned int i;

  if (align <= 1)
    return value;

  for (i = 1, l = 0; (l < 32) && (i < (unsigned int)align); l++)
    {
      i <<= 1;
    }
  align = i;
  value = (value + align - 1) / align;
  return value * align;
}
