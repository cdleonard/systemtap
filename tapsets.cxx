// tapset resolution
// Copyright (C) 2005-2015 Red Hat Inc.
// Copyright (C) 2005-2007 Intel Corporation.
// Copyright (C) 2008 James.Bottomley@HansenPartnership.com
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "elaborate.h"
#include "tapsets.h"
#include "task_finder.h"
#include "tapset-dynprobe.h"
#include "translate.h"
#include "session.h"
#include "util.h"
#include "buildrun.h"
#include "dwarf_wrappers.h"
#include "auto_free.h"
#include "hash.h"
#include "dwflpp.h"
#include "setupdwfl.h"
#include <gelf.h>

#include "sdt_types.h"

#include <cstdlib>
#include <algorithm>
#include <deque>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <stack>
#include <cstdarg>
#include <cassert>
#include <iomanip>
#include <cerrno>

extern "C" {
#include <fcntl.h>
#include <elfutils/libdwfl.h>
#include <elfutils/libdw.h>
#include <dwarf.h>
#include <elf.h>
#include <obstack.h>
#include <glob.h>
#include <fnmatch.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <regex.h>
#include <unistd.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
}

using namespace std;
using namespace __gnu_cxx;

// for elf.h where PPC64_LOCAL_ENTRY_OFFSET isn't defined
#ifndef PPC64_LOCAL_ENTRY_OFFSET
#define STO_PPC64_LOCAL_BIT    5
#define STO_PPC64_LOCAL_MASK   (7 << STO_PPC64_LOCAL_BIT)
#define PPC64_LOCAL_ENTRY_OFFSET(other)					\
 (((1 << (((other) & STO_PPC64_LOCAL_MASK) >> STO_PPC64_LOCAL_BIT)) >> 2) << 2)
#endif
// for elf.h where EF_PPC64_ABI isn't defined
#ifndef EF_PPC64_ABI
#define EF_PPC64_ABI 3
#endif

// ------------------------------------------------------------------------

string
common_probe_init (derived_probe* p)
{
  assert(p->session_index != (unsigned)-1);
  return "(&stap_probes[" + lex_cast(p->session_index) + "])";
}


void
common_probe_entryfn_prologue (systemtap_session& s,
			       string statestr, string probe,
			       string probe_type, bool overload_processing)
{
  if (s.runtime_usermode_p())
    {
      // If session_state() is NULL, then we haven't even initialized shm yet,
      // and there's *nothing* for the probe to do.  (even alibi is in shm)
      // So failure skips this whole block through the end of the epilogue.
      s.op->newline() << "if (likely(session_state())) {";
      s.op->indent(1);
    }

  s.op->newline() << "#ifdef STP_ALIBI";
  s.op->newline() << "atomic_inc(probe_alibi(" << probe << "->index));";
  s.op->newline() << "#else";

  if (s.runtime_usermode_p())
    s.op->newline() << "int _stp_saved_errno = errno;";

  s.op->newline() << "struct context* __restrict__ c = NULL;";
  s.op->newline() << "#if !INTERRUPTIBLE";
  s.op->newline() << "unsigned long flags;";
  s.op->newline() << "#endif";

  s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "Stat stat = probe_timing(" << probe << "->index);";
  s.op->newline() << "#endif";
  if (overload_processing && !s.runtime_usermode_p())
    s.op->newline() << "#if defined(STP_TIMING) || defined(STP_OVERLOAD)";
  else
    s.op->newline() << "#ifdef STP_TIMING";

  if (! s.runtime_usermode_p())
    s.op->newline() << "cycles_t cycles_atstart = get_cycles ();";
  else
    {
    s.op->newline() << "struct timespec timespec_atstart;";
    s.op->newline() << "(void)clock_gettime(CLOCK_MONOTONIC, &timespec_atstart);";
    }
  s.op->newline() << "#endif";

  s.op->newline() << "#if !INTERRUPTIBLE";
  s.op->newline() << "local_irq_save (flags);";
  s.op->newline() << "#endif";

  if (! s.runtime_usermode_p())
    {
      // Check for enough free enough stack space
      s.op->newline() << "if (unlikely ((((unsigned long) (& c)) & (THREAD_SIZE-1))"; // free space
      s.op->newline(1) << "< (MINSTACKSPACE + sizeof (struct thread_info)))) {"; // needed space
      // XXX: may need porting to platforms where task_struct is not
      // at bottom of kernel stack NB: see also
      // CONFIG_DEBUG_STACKOVERFLOW
      s.op->newline() << "atomic_inc (skipped_count());";
      s.op->newline() << "#ifdef STP_TIMING";
      s.op->newline() << "atomic_inc (skipped_count_lowstack());";
      s.op->newline() << "#endif";
      s.op->newline() << "goto probe_epilogue;";
      s.op->newline(-1) << "}";
    }

  s.op->newline() << "if (atomic_read (session_state()) != " << statestr << ")";
  s.op->newline(1) << "goto probe_epilogue;";
  s.op->indent(-1);

  s.op->newline() << "c = _stp_runtime_entryfn_get_context();";
  s.op->newline() << "if (!c) {";
  s.op->newline(1) << "#if !INTERRUPTIBLE";
  s.op->newline() << "atomic_inc (skipped_count());";
  s.op->newline() << "#endif";
  s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "atomic_inc (skipped_count_reentrant());";
  s.op->newline() << "#endif";
  s.op->newline() << "goto probe_epilogue;";
  s.op->newline(-1) << "}";

  s.op->newline();
  s.op->newline() << "c->last_stmt = 0;";
  s.op->newline() << "c->last_error = 0;";
  s.op->newline() << "c->nesting = -1;"; // NB: PR10516 packs locals[] tighter
  s.op->newline() << "c->uregs = 0;";
  s.op->newline() << "c->kregs = 0;";
  s.op->newline() << "#if defined __ia64__";
  s.op->newline() << "c->unwaddr = 0;";
  s.op->newline() << "#endif";
  if (s.runtime_usermode_p())
    s.op->newline() << "c->probe_index = " << probe << "->index;";
  s.op->newline() << "c->probe_point = " << probe << "->pp;";
  s.op->newline() << "#ifdef STP_NEED_PROBE_NAME";
  s.op->newline() << "c->probe_name = " << probe << "->pn;";
  s.op->newline() << "#endif";
  s.op->newline() << "c->probe_type = " << probe_type << ";";
  // reset Individual Probe State union
  s.op->newline() << "memset(&c->ips, 0, sizeof(c->ips));";
  s.op->newline() << "c->user_mode_p = 0; c->full_uregs_p = 0;";
  s.op->newline() << "#ifdef STAP_NEED_REGPARM"; // i386 or x86_64 register.stp
  s.op->newline() << "c->regparm = 0;";
  s.op->newline() << "#endif";

  if(!s.suppress_time_limits){
    s.op->newline() << "#if INTERRUPTIBLE";
    s.op->newline() << "c->actionremaining = MAXACTION_INTERRUPTIBLE;";
    s.op->newline() << "#else";
    s.op->newline() << "c->actionremaining = MAXACTION;";
    s.op->newline() << "#endif";
  }
  // NB: The following would actually be incorrect.
  // That's because cycles_sum/cycles_base values are supposed to survive
  // between consecutive probes.  Periodically (STP_OVERLOAD_INTERVAL
  // cycles), the values will be reset.
  /*
  s.op->newline() << "#ifdef STP_OVERLOAD";
  s.op->newline() << "c->cycles_sum = 0;";
  s.op->newline() << "c->cycles_base = 0;";
  s.op->newline() << "#endif";
  */

  s.op->newline() << "#if defined(STP_NEED_UNWIND_DATA)";
  s.op->newline() << "c->uwcache_user.state = uwcache_uninitialized;";
  s.op->newline() << "c->uwcache_kernel.state = uwcache_uninitialized;";
  s.op->newline() << "#endif";
}


void
common_probe_entryfn_epilogue (systemtap_session& s,
                               bool overload_processing,
                               bool schedule_work_safe)
{
  if (!s.runtime_usermode_p()
      && schedule_work_safe)
    {
      // If a refresh is required, we can safely schedule_work() here
      s.op->newline( 0) <<  "if (atomic_cmpxchg(&need_module_refresh, 1, 0) == 1)";
      s.op->newline(+1) <<    "schedule_work(&module_refresher_work);";
      s.op->indent(-1);
    }

  if (overload_processing && !s.runtime_usermode_p())
    s.op->newline() << "#if defined(STP_TIMING) || defined(STP_OVERLOAD)";
  else
    s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "{";
  s.op->indent(1);
  if (! s.runtime_usermode_p())
    {
      s.op->newline() << "cycles_t cycles_atend = get_cycles ();";
      // NB: we truncate cycles counts to 32 bits.  Perhaps it should be
      // fewer, if the hardware counter rolls over really quickly.  We
      // handle 32-bit wraparound here.
      s.op->newline() << "int32_t cycles_elapsed = ((int32_t)cycles_atend > (int32_t)cycles_atstart)";
      s.op->newline(1) << "? ((int32_t)cycles_atend - (int32_t)cycles_atstart)";
      s.op->newline() << ": (~(int32_t)0) - (int32_t)cycles_atstart + (int32_t)cycles_atend + 1;";
      s.op->indent(-1);
    }
  else
    {
      s.op->newline() << "struct timespec timespec_atend, timespec_elapsed;";
      s.op->newline() << "long cycles_elapsed;";
      s.op->newline() << "(void)clock_gettime(CLOCK_MONOTONIC, &timespec_atend);";
      s.op->newline() << "_stp_timespec_sub(&timespec_atend, &timespec_atstart, &timespec_elapsed);";
      // 'cycles_elapsed' is really elapsed nanoseconds
      s.op->newline() << "cycles_elapsed = (timespec_elapsed.tv_sec * NSEC_PER_SEC) + timespec_elapsed.tv_nsec;";
    }

  s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "if (likely (stat)) _stp_stat_add(stat, cycles_elapsed);";
  s.op->newline() << "#endif";

  if (overload_processing && !s.runtime_usermode_p())
    {
      s.op->newline() << "#ifdef STP_OVERLOAD";
      s.op->newline() << "{";
      // If the cycle count has wrapped (cycles_atend > cycles_base),
      // let's go ahead and pretend the interval has been reached.
      // This should reset cycles_base and cycles_sum.
      s.op->newline(1) << "cycles_t interval = (cycles_atend > c->cycles_base)";
      s.op->newline(1) << "? (cycles_atend - c->cycles_base)";
      s.op->newline() << ": (STP_OVERLOAD_INTERVAL + 1);";
      s.op->newline(-1) << "c->cycles_sum += cycles_elapsed;";

      // If we've spent more than STP_OVERLOAD_THRESHOLD cycles in a
      // probe during the last STP_OVERLOAD_INTERVAL cycles, the probe
      // has overloaded the system and we need to quit.
      // NB: this is not suppressible via --suppress-runtime-errors,
      // because this is a system safety metric that we cannot trust
      // unprivileged users to override.
      s.op->newline() << "if (interval > STP_OVERLOAD_INTERVAL) {";
      s.op->newline(1) << "if (c->cycles_sum > STP_OVERLOAD_THRESHOLD) {";
      s.op->newline(1) << "_stp_error (\"probe overhead exceeded threshold\");";
      s.op->newline() << "atomic_set (session_state(), STAP_SESSION_ERROR);";
      s.op->newline() << "atomic_inc (error_count());";
      s.op->newline(-1) << "}";

      s.op->newline() << "c->cycles_base = cycles_atend;";
      s.op->newline() << "c->cycles_sum = 0;";
      s.op->newline(-1) << "}";
      s.op->newline(-1) << "}";
      s.op->newline() << "#endif";
    }

  s.op->newline(-1) << "}";
  s.op->newline() << "#endif";

  s.op->newline() << "c->probe_point = 0;"; // vacated
  s.op->newline() << "#ifdef STP_NEED_PROBE_NAME";
  s.op->newline() << "c->probe_name = 0;";
  s.op->newline() << "#endif";
  s.op->newline() << "c->probe_type = 0;";


  s.op->newline() << "if (unlikely (c->last_error && c->last_error[0])) {";
  s.op->indent(1);
  if (s.suppress_handler_errors) // PR 13306
    {
      s.op->newline() << "atomic_inc (error_count());";
    }
  else
    {
      s.op->newline() << "if (c->last_stmt != NULL)";
      s.op->newline(1) << "_stp_softerror (\"%s near %s\", c->last_error, c->last_stmt);";
      s.op->newline(-1) << "else";
      s.op->newline(1) << "_stp_softerror (\"%s\", c->last_error);";
      s.op->indent(-1);
      s.op->newline() << "atomic_inc (error_count());";
      s.op->newline() << "if (atomic_read (error_count()) > MAXERRORS) {";
      s.op->newline(1) << "atomic_set (session_state(), STAP_SESSION_ERROR);";
      s.op->newline() << "_stp_exit ();";
      s.op->newline(-1) << "}";
    }

  s.op->newline(-1) << "}";


  s.op->newline(-1) << "probe_epilogue:"; // context is free
  s.op->indent(1);

  if (! s.suppress_handler_errors) // PR 13306
    {
      // Check for excessive skip counts.
      s.op->newline() << "if (unlikely (atomic_read (skipped_count()) > MAXSKIPPED)) {";
      s.op->newline(1) << "if (unlikely (pseudo_atomic_cmpxchg(session_state(), STAP_SESSION_RUNNING, STAP_SESSION_ERROR) == STAP_SESSION_RUNNING))";
      s.op->newline() << "_stp_error (\"Skipped too many probes, check MAXSKIPPED or try again with stap -t for more details.\");";
      s.op->newline(-1) << "}";
    }

  // We mustn't release the context until after all _stp_error(), so dyninst
  // mode can still access the log buffers stored therein.
  s.op->newline() << "_stp_runtime_entryfn_put_context(c);";

  s.op->newline() << "#if !INTERRUPTIBLE";
  s.op->newline() << "local_irq_restore (flags);";
  s.op->newline() << "#endif";

  if (s.runtime_usermode_p())
    {
      s.op->newline() << "errno = _stp_saved_errno;";
    }

  s.op->newline() << "#endif // STP_ALIBI";

  if (s.runtime_usermode_p())
    s.op->newline(-1) << "}";
}


// ------------------------------------------------------------------------

// ------------------------------------------------------------------------
//  Dwarf derived probes.  "We apologize for the inconvience."
// ------------------------------------------------------------------------

static const string TOK_KERNEL("kernel");
static const string TOK_MODULE("module");
static const string TOK_FUNCTION("function");
static const string TOK_INLINE("inline");
static const string TOK_CALL("call");
static const string TOK_EXPORTED("exported");
static const string TOK_RETURN("return");
static const string TOK_MAXACTIVE("maxactive");
static const string TOK_STATEMENT("statement");
static const string TOK_ABSOLUTE("absolute");
static const string TOK_PROCESS("process");
static const string TOK_PROVIDER("provider");
static const string TOK_MARK("mark");
static const string TOK_TRACE("trace");
static const string TOK_LABEL("label");
static const string TOK_LIBRARY("library");
static const string TOK_PLT("plt");
static const string TOK_METHOD("method");
static const string TOK_CLASS("class");;
static const string TOK_CALLEE("callee");;
static const string TOK_CALLEES("callees");;
static const string TOK_NEAREST("nearest");;

// Can we handle this query with just symbol-table info?
enum dbinfo_reqt
{
  dbr_unknown,
  dbr_none,		// kernel.statement(NUM).absolute
  dbr_need_symtab,	// can get by with symbol table if there's no dwarf
  dbr_need_dwarf
};


struct dwarf_query; // forward decl

static int query_cu (Dwarf_Die * cudie, dwarf_query *q);
static void query_addr(Dwarf_Addr addr, dwarf_query *q);
static void query_plt_statement(dwarf_query *q);

struct
symbol_table
{
  module_info *mod_info;	// associated module
  multimap<string, func_info*> map_by_name;
  multimap<Dwarf_Addr, func_info*> map_by_addr;
  map<string, Dwarf_Addr> globals;
  map<string, Dwarf_Addr> locals;
  typedef multimap<Dwarf_Addr, func_info*>::iterator iterator_t;
  typedef pair<iterator_t, iterator_t> range_t;
  // Section describing function descriptors.
  // Set to SHN_UNDEF if there is no such section.
  GElf_Word opd_section;
  void add_symbol(const char *name, bool weak, bool descriptor,
                  Dwarf_Addr addr, Dwarf_Addr entrypc);
  enum info_status get_from_elf();
  void prepare_section_rejection(Dwfl_Module *mod);
  bool reject_section(GElf_Word section);
  void purge_syscall_stubs();
  set <func_info*> lookup_symbol(const string& name);
  set <Dwarf_Addr> lookup_symbol_address(const string& name);
  func_info *get_func_containing_address(Dwarf_Addr addr);
  func_info *get_first_func();

  symbol_table(module_info *mi) : mod_info(mi), opd_section(SHN_UNDEF) {}
  ~symbol_table();
};

static bool null_die(Dwarf_Die *die)
{
  static Dwarf_Die null;
  return (!die || !memcmp(die, &null, sizeof(null)));
}

struct exp_type_dwarf : public exp_type_details
{
  // NB: We don't own this dwflpp, so don't use it after build_no_more!
  // A shared_ptr might help, but expressions are currently so leaky
  // that we'd probably never clear all references... :/
  dwflpp* dw;
  Dwarf_Die die;
  bool userspace_p;
  bool is_pointer;
  exp_type_dwarf(dwflpp* dw, Dwarf_Die* die, bool userspace_p, bool addressof);
  uintptr_t id () const { return reinterpret_cast<uintptr_t>(die.addr); }
  bool expandable() const { return true; }
  functioncall *expand(autocast_op* e, bool lvalue);
};


enum
function_spec_type
  {
    function_alone,
    function_and_file,
    function_file_and_line
  };


struct dwarf_builder;
struct dwarf_var_expanding_visitor;


// XXX: This class is a candidate for subclassing to separate
// the relocation vs non-relocation variants.  Likewise for
// kprobe vs kretprobe variants.

struct dwarf_derived_probe: public derived_probe
{
  dwarf_derived_probe (const string& function,
                       const string& filename,
                       int line,
                       const string& module,
                       const string& section,
		       Dwarf_Addr dwfl_addr,
		       Dwarf_Addr addr,
		       dwarf_query & q,
                       Dwarf_Die* scope_die);

  string module;
  string section;
  Dwarf_Addr addr;
  string path;
  bool has_process;
  bool has_return;
  bool has_maxactive;
  bool has_library;
  long maxactive_val;
  // dwarf_derived_probe_group::emit_module_decls uses this to emit sdt kprobe definition
  string user_path;
  string user_lib;
  bool access_vars;

  unsigned saved_longs, saved_strings;
  dwarf_derived_probe* entry_handler;

  void printsig (std::ostream &o) const;
  virtual void join_group (systemtap_session& s);
  void emit_probe_local_init(systemtap_session& s, translator_output * o);
  void getargs(std::list<std::string> &arg_set) const;

  void emit_privilege_assertion (translator_output*);
  void print_dupe_stamp(ostream& o);

  // Pattern registration helpers.
  static void register_statement_variants(match_node * root,
					  dwarf_builder * dw,
					  privilege_t privilege);
  static void register_function_variants(match_node * root,
					 dwarf_builder * dw,
					 privilege_t privilege);
  static void register_function_and_statement_variants(systemtap_session& s,
						       match_node * root,
						       dwarf_builder * dw,
						       privilege_t privilege);
  static void register_sdt_variants(systemtap_session& s,
				    match_node * root,
				    dwarf_builder * dw);
  static void register_plt_variants(systemtap_session& s,
				    match_node * root,
				    dwarf_builder * dw);
  static void register_patterns(systemtap_session& s);

protected:
  dwarf_derived_probe(probe *base,
                      probe_point *location,
                      Dwarf_Addr addr,
                      bool has_return):
    derived_probe(base, location), addr(addr), has_process(0),
    has_return(has_return), has_maxactive(0), has_library(0),
    maxactive_val(0), access_vars(false), saved_longs(0),
    saved_strings(0), entry_handler(0)
  {}

private:
  list<string> args;
  void saveargs(dwarf_query& q, Dwarf_Die* scope_die, Dwarf_Addr dwfl_addr);
};


struct uprobe_derived_probe: public dwarf_derived_probe
{
  int pid; // 0 => unrestricted

  uprobe_derived_probe (const string& function,
                        const string& filename,
                        int line,
                        const string& module,
                        const string& section,
                        Dwarf_Addr dwfl_addr,
                        Dwarf_Addr addr,
                        dwarf_query & q,
                        Dwarf_Die* scope_die);

  // alternate constructor for process(PID).statement(ADDR).absolute
  uprobe_derived_probe (probe *base,
                        probe_point *location,
                        int pid,
                        Dwarf_Addr addr,
                        bool has_return):
    dwarf_derived_probe(base, location, addr, has_return), pid(pid)
  {}

  void join_group (systemtap_session& s);

  void emit_privilege_assertion (translator_output*);
  void print_dupe_stamp(ostream& o) { print_dupe_stamp_unprivileged_process_owner (o); }
  void getargs(std::list<std::string> &arg_set) const;
  void saveargs(int nargs);
private:
  list<string> args;
};

struct dwarf_derived_probe_group: public derived_probe_group
{
private:
  multimap<string,dwarf_derived_probe*> probes_by_module;
  typedef multimap<string,dwarf_derived_probe*>::iterator p_b_m_iterator;

public:
  dwarf_derived_probe_group() {}
  void enroll (dwarf_derived_probe* probe);
  void emit_module_decls (systemtap_session& s);
  void emit_module_init (systemtap_session& s);
  void emit_module_refresh (systemtap_session& s);
  void emit_module_exit (systemtap_session& s);
  bool otf_supported (systemtap_session& s) { return true; }

  // workqueue handling not safe in kprobes context
  bool otf_safe_context (systemtap_session& s) { return false; }
};

// Helper struct to thread through the dwfl callbacks.
struct base_query
{
  base_query(dwflpp & dw, literal_map_t const & params);
  base_query(dwflpp & dw, const string & module_val);
  virtual ~base_query() {}

  systemtap_session & sess;
  dwflpp & dw;

  // Used to keep track of which modules were visited during
  // iterate_over_modules()
  set<string> visited_modules;

  // Parameter extractors.
  static bool has_null_param(literal_map_t const & params,
                             string const & k);
  static bool get_string_param(literal_map_t const & params,
			       string const & k, string & v);
  static bool get_number_param(literal_map_t const & params,
			       string const & k, long & v);
  static bool get_number_param(literal_map_t const & params,
			       string const & k, long long & v);
  static bool get_number_param(literal_map_t const & params,
			       string const & k, Dwarf_Addr & v);
  static void query_library_callback (base_query *me, const char *data);
  static void query_plt_callback (base_query *me, const char *link, size_t addr);
  virtual void query_library (const char *data) = 0;
  virtual void query_plt (const char *link, size_t addr) = 0;


  // Extracted parameters.
  bool has_kernel;
  bool has_module;
  bool has_process;
  bool has_library;
  bool has_plt;
  bool has_statement;
  string module_val; // has_kernel => module_val = "kernel"
  string path;	     // executable path if module is a .so
  string plt_val;    // has_plt => plt wildcard
  int64_t pid_val;

  virtual void handle_query_module() = 0;
};

base_query::base_query(dwflpp & dw, literal_map_t const & params):
  sess(dw.sess), dw(dw), has_library(false), has_plt(false), has_statement(false),
  pid_val(0)
{
  has_kernel = has_null_param (params, TOK_KERNEL);
  if (has_kernel)
    module_val = "kernel";

  has_module = get_string_param (params, TOK_MODULE, module_val);
  if (has_module)
    has_process = false;
  else
    {
      string library_name;
      long statement_num_val;
      has_process =  derived_probe_builder::has_param(params, TOK_PROCESS);
      has_library = get_string_param (params, TOK_LIBRARY, library_name);
      if ((has_plt = has_null_param (params, TOK_PLT)))
        plt_val = "*";
      else has_plt = get_string_param (params, TOK_PLT, plt_val);
      has_statement = get_number_param(params, TOK_STATEMENT, statement_num_val);

      if (has_process)
        {
          if (get_number_param(params, TOK_PROCESS, pid_val))
            {
              // check that the pid given corresponds to a running process
              string pid_err_msg;
              if (!is_valid_pid(pid_val, pid_err_msg))
                throw SEMANTIC_ERROR(pid_err_msg);

              string pid_path = string("/proc/") + lex_cast(pid_val) + "/exe";
              module_val = sess.sysroot + pid_path;
            }
          else 
            {
              // reset the pid_val in case anything weird got written into it
              pid_val = 0;
              get_string_param(params, TOK_PROCESS, module_val);
            }
          module_val = find_executable (module_val, sess.sysroot, sess.sysenv);
          if (!is_fully_resolved(module_val, sess.sysroot, sess.sysenv))
            throw SEMANTIC_ERROR(_F("cannot find executable '%s'",
                                    module_val.c_str()));
        }

      // Library probe? Let's target that instead if it is fully resolved (such
      // as what query_one_library() would have done for us). Otherwise, we
      // resort to iterate_over_libraries().
      if (has_library && is_fully_resolved(library_name, sess.sysroot, sess.sysenv,
                                           "LD_LIBRARY_PATH"))
        {
          path = path_remove_sysroot(sess, module_val);
          module_val = library_name;
        }
    }

  assert (has_kernel || has_process || has_module);
}

base_query::base_query(dwflpp & dw, const string & module_val)
  : sess(dw.sess), dw(dw), has_library(false), has_plt(false), has_statement(false),
    module_val(module_val), pid_val(0)
{
  // NB: This uses '/' to distinguish between kernel modules and userspace,
  // which means that userspace modules won't get any PATH searching.
  if (module_val.find('/') == string::npos)
    {
      has_kernel = (module_val == TOK_KERNEL);
      has_module = !has_kernel;
      has_process = false;
    }
  else
    {
      has_kernel = has_module = false;
      has_process = true;
    }
}

bool
base_query::has_null_param(literal_map_t const & params,
			   string const & k)
{
  return derived_probe_builder::has_null_param(params, k);
}


bool
base_query::get_string_param(literal_map_t const & params,
			     string const & k, string & v)
{
  return derived_probe_builder::get_param (params, k, v);
}


bool
base_query::get_number_param(literal_map_t const & params,
			     string const & k, long & v)
{
  int64_t value;
  bool present = derived_probe_builder::get_param (params, k, value);
  v = (long) value;
  return present;
}


bool
base_query::get_number_param(literal_map_t const & params,
			     string const & k, long long & v)
{
  int64_t value;
  bool present = derived_probe_builder::get_param (params, k, value);
  v = (long) value;
  return present;
}


bool
base_query::get_number_param(literal_map_t const & params,
			     string const & k, Dwarf_Addr & v)
{
  int64_t value;
  bool present = derived_probe_builder::get_param (params, k, value);
  v = (Dwarf_Addr) value;
  return present;
}

struct dwarf_query : public base_query
{
  dwarf_query(probe * base_probe,
	      probe_point * base_loc,
	      dwflpp & dw,
	      literal_map_t const & params,
	      vector<derived_probe *> & results,
	      const string user_path,
	      const string user_lib);

  vector<derived_probe *> & results;
  set<string> inlined_non_returnable; // function names
  probe * base_probe;
  probe_point * base_loc;
  string user_path;
  string user_lib;

  set<string> visited_libraries;
  bool resolved_library;

  virtual void handle_query_module();
  void query_module_dwarf();
  void query_module_symtab();
  void query_library (const char *data);
  void query_plt (const char *entry, size_t addr);

  void add_probe_point(string const & funcname,
		       char const * filename,
		       int line,
		       Dwarf_Die *scope_die,
		       Dwarf_Addr addr);

  void mount_well_formed_probe_point();
  void unmount_well_formed_probe_point();
  stack<pair<probe_point*, probe*> > previous_bases;

  void replace_probe_point_component_arg(const string& functor,
                                         const string& new_functor,
                                         int64_t new_arg,
                                         bool hex = false);
  void replace_probe_point_component_arg(const string& functor,
                                         int64_t new_arg,
                                         bool hex = false);
  void replace_probe_point_component_arg(const string& functor,
                                         const string& new_functor,
                                         const string& new_arg);
  void replace_probe_point_component_arg(const string& functor,
                                         const string& new_arg);
  void remove_probe_point_component(const string& functor);

  // Track addresses we've already seen in a given module
  set<Dwarf_Addr> alias_dupes;

  // Track inlines we've already seen as well
  // NB: this can't be compared just by entrypc, as inlines can overlap
  set<inline_instance_info> inline_dupes;

  // Used in .callee[s] probes, when calling iterate_over_callees() (which
  // provides the actual stack). Retains the addrs of the callers unwind addr
  // where the callee is found. Specifies multiple callers. E.g. when a callee
  // at depth 2 is found, callers[1] has the addr of the caller, and callers[0]
  // has the addr of the caller's caller.
  stack<Dwarf_Addr> *callers;

  bool has_function_str;
  bool has_statement_str;
  bool has_function_num;
  bool has_statement_num;
  string statement_str_val;
  string function_str_val;
  Dwarf_Addr statement_num_val;
  Dwarf_Addr function_num_val;

  bool has_call;
  bool has_exported;
  bool has_inline;
  bool has_return;

  bool has_nearest;

  bool has_maxactive;
  long maxactive_val;

  bool has_label;
  string label_val;

  bool has_callee;
  string callee_val;

  bool has_callees_num;
  long callees_num_val;

  bool has_relative;
  long relative_val;

  bool has_absolute;

  bool has_mark;

  enum dbinfo_reqt dbinfo_reqt;
  enum dbinfo_reqt assess_dbinfo_reqt();

  void parse_function_spec(const string & spec);
  function_spec_type spec_type;
  vector<string> scopes;
  string function;
  string file;
  lineno_t lineno_type;
  vector<int> linenos;
  bool query_done;	// Found exact match

  // Holds the prologue end of the current function
  Dwarf_Addr prologue_end;

  set<string> filtered_srcfiles;

  // Map official entrypc -> func_info object
  inline_instance_map_t filtered_inlines;
  func_info_map_t filtered_functions;

  // Helper when we want to iterate over both
  base_func_info_map_t filtered_all();

  void query_module_functions ();

  string final_function_name(const string& final_func,
                             const char* final_file,
                             int final_line);

  bool is_fully_specified_function();
};


uprobe_derived_probe::uprobe_derived_probe (const string& function,
                        const string& filename,
                        int line,
                        const string& module,
                        const string& section,
                        Dwarf_Addr dwfl_addr,
                        Dwarf_Addr addr,
                        dwarf_query & q,
                        Dwarf_Die* scope_die):
    dwarf_derived_probe(function, filename, line, module, section,
                        dwfl_addr, addr, q, scope_die), pid(q.pid_val)
  {}


static void delete_session_module_cache (systemtap_session& s); // forward decl

struct dwarf_builder: public derived_probe_builder
{
  map <string,dwflpp*> kern_dw; /* NB: key string could be a wildcard */
  map <string,dwflpp*> user_dw;
  string user_path;
  string user_lib;

  // Holds modules to suggest functions from. NB: aggregates over
  // recursive calls to build() when deriving globby probes.
  set <string> modules_seen;

  dwarf_builder() {}

  dwflpp *get_kern_dw(systemtap_session& sess, const string& module)
  {
    if (kern_dw[module] == 0)
      kern_dw[module] = new dwflpp(sess, module, true); // might throw
    return kern_dw[module];
  }

  dwflpp *get_user_dw(systemtap_session& sess, const string& module)
  {
    if (user_dw[module] == 0)
      user_dw[module] = new dwflpp(sess, module, false); // might throw
    return user_dw[module];
  }

  /* NB: not virtual, so can be called from dtor too: */
  void dwarf_build_no_more (bool)
  {
    delete_map(kern_dw);
    delete_map(user_dw);
  }

  void build_no_more (systemtap_session &s)
  {
    dwarf_build_no_more (s.verbose > 3);
    delete_session_module_cache (s);
  }

  ~dwarf_builder()
  {
    dwarf_build_no_more (false);
  }

  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     literal_map_t const & parameters,
		     vector<derived_probe *> & finished_results);
};


dwarf_query::dwarf_query(probe * base_probe,
			 probe_point * base_loc,
			 dwflpp & dw,
			 literal_map_t const & params,
			 vector<derived_probe *> & results,
			 const string user_path,
			 const string user_lib)
  : base_query(dw, params), results(results), base_probe(base_probe),
    base_loc(base_loc), user_path(user_path), user_lib(user_lib),
    resolved_library(false), callers(NULL), has_relative(false),
    relative_val(0), prologue_end(0)
{
  // Reduce the query to more reasonable semantic values (booleans,
  // extracted strings, numbers, etc).
  has_function_str = get_string_param(params, TOK_FUNCTION, function_str_val);
  has_function_num = get_number_param(params, TOK_FUNCTION, function_num_val);

  has_statement_str = get_string_param(params, TOK_STATEMENT, statement_str_val);
  has_statement_num = get_number_param(params, TOK_STATEMENT, statement_num_val);

  has_label = get_string_param(params, TOK_LABEL, label_val);
  has_callee = get_string_param(params, TOK_CALLEE, callee_val);
  if (has_null_param(params, TOK_CALLEES))
    { // .callees ==> .callees(1) (also equivalent to .callee("*"))
      has_callees_num = true;
      callees_num_val = 1;
    }
  else
    {
      has_callees_num = get_number_param(params, TOK_CALLEES, callees_num_val);
      if (has_callees_num && callees_num_val < 1)
        throw SEMANTIC_ERROR(_(".callees(N) only acceptable for N >= 1"),
                             base_probe->tok);
    }

  has_call = has_null_param(params, TOK_CALL);
  has_exported = has_null_param(params, TOK_EXPORTED);
  has_inline = has_null_param(params, TOK_INLINE);
  has_return = has_null_param(params, TOK_RETURN);
  has_nearest = has_null_param(params, TOK_NEAREST);
  has_maxactive = get_number_param(params, TOK_MAXACTIVE, maxactive_val);
  has_absolute = has_null_param(params, TOK_ABSOLUTE);
  has_mark = false;

  if (has_function_str)
    parse_function_spec(function_str_val);
  else if (has_statement_str)
    parse_function_spec(statement_str_val);

  dbinfo_reqt = assess_dbinfo_reqt();
  query_done = false;
}


void
dwarf_query::query_module_dwarf()
{
  if (has_function_num || has_statement_num)
    {
      // If we have module("foo").function(0xbeef) or
      // module("foo").statement(0xbeef), the address is relative
      // to the start of the module, so we seek the function
      // number plus the module's bias.
      Dwarf_Addr addr = has_function_num ?
        function_num_val : statement_num_val;

      // These are raw addresses, we need to know what the elf_bias
      // is to feed it to libdwfl based functions.
      Dwarf_Addr elf_bias;
      Elf *elf = dwfl_module_getelf (dw.module, &elf_bias);
      assert(elf);
      addr += elf_bias;
      query_addr(addr, this);
    }
  else
    {
      // Otherwise if we have a function("foo") or statement("foo")
      // specifier, we have to scan over all the CUs looking for
      // the function(s) in question
      assert(has_function_str || has_statement_str);

      // For simple cases, no wildcard and no source:line, we can do a very
      // quick function lookup in a module-wide cache.
      if (spec_type == function_alone &&
          !dw.name_has_wildcard(function) &&
          !startswith(function, "_Z"))
        query_module_functions();
      else
        dw.iterate_over_cus(&query_cu, this, false);
    }
}

static void query_func_info (Dwarf_Addr entrypc, func_info & fi,
							dwarf_query * q);

static void
query_symtab_func_info (func_info & fi, dwarf_query * q)
{
  assert(null_die(&fi.die));

  Dwarf_Addr entrypc = fi.entrypc;

  // Now compensate for the dw bias because the addresses come
  // from dwfl_module_symtab, so fi->entrypc is NOT a normal dw address.
  q->dw.get_module_dwarf(false, false);
  entrypc -= q->dw.module_bias;

  // If there are already probes in this module, lets not duplicate.
  // This can come from other weak symbols/aliases or existing
  // matches from Dwarf DIE functions.  Try to add this entrypc to the
  // collection, and only continue if it was new.
  if (q->alias_dupes.insert(entrypc).second)
    query_func_info(entrypc, fi, q);
}

void
dwarf_query::query_module_symtab()
{
  // Get the symbol table if it's necessary, sufficient, and not already got.
  if (dbinfo_reqt == dbr_need_dwarf)
    return;

  module_info *mi = dw.mod_info;
  if (dbinfo_reqt == dbr_need_symtab)
    {
      if (mi->symtab_status == info_unknown)
        mi->get_symtab();
      if (mi->symtab_status == info_absent)
        return;
    }

  func_info *fi = NULL;
  symbol_table *sym_table = mi->sym_table;

  if (has_function_str)
    {
      // Per dwarf_query::assess_dbinfo_reqt()...
      assert(spec_type == function_alone);
      if (dw.name_has_wildcard(function_str_val))
        {
          symbol_table::iterator_t iter;
          for (iter = sym_table->map_by_addr.begin();
               iter != sym_table->map_by_addr.end();
               ++iter)
            {
              fi = iter->second;
              if (!null_die(&fi->die) // already handled in query_module_dwarf()
                  || fi->descriptor) // ppc opd (and also undefined symbols)
                continue;
              if (dw.function_name_matches_pattern(fi->name, function_str_val))
                query_symtab_func_info(*fi, this);
            }
        }
      else
        {
          set<func_info*> fis = sym_table->lookup_symbol(function_str_val);
          for (set<func_info*>::iterator it=fis.begin(); it!=fis.end(); ++it)
            {
              fi = *it;
              if (fi && null_die(&fi->die))
                query_symtab_func_info(*fi, this);
            }
        }
    }
}

void
dwarf_query::handle_query_module()
{
  if (has_plt && has_statement_num)
    {
      query_plt_statement (this);
      return;
    }

  bool report = dbinfo_reqt == dbr_need_dwarf;
  dw.get_module_dwarf(false, report);

  // prebuild the symbol table to resolve aliases
  dw.mod_info->get_symtab();

  // reset the dupe-checking for each new module
  alias_dupes.clear();
  inline_dupes.clear();

  if (dw.mod_info->dwarf_status == info_present)
    query_module_dwarf();

  // Consult the symbol table, asm and weak functions can show up
  // in the symbol table but not in dwarf and minidebuginfo is
  // located in the gnu_debugdata section, alias_dupes checking
  // is done before adding any probe points
  if (!query_done && !pending_interrupts)
    query_module_symtab();
}


void
dwarf_query::parse_function_spec(const string & spec)
{
  lineno_type = ABSOLUTE;
  size_t src_pos, line_pos, scope_pos;

  // look for named scopes
  scope_pos = spec.rfind("::");
  if (scope_pos != string::npos)
    {
      tokenize_cxx(spec.substr(0, scope_pos), scopes);
      scope_pos += 2;
    }
  else
    scope_pos = 0;

  // look for a source separator
  src_pos = spec.find('@', scope_pos);
  if (src_pos == string::npos)
    {
      function = spec.substr(scope_pos);
      spec_type = function_alone;
    }
  else
    {
      function = spec.substr(scope_pos, src_pos - scope_pos);

      // look for a line-number separator
      line_pos = spec.find_first_of(":+", src_pos);
      if (line_pos == string::npos)
        {
          file = spec.substr(src_pos + 1);
          spec_type = function_and_file;
        }
      else
        {
          file = spec.substr(src_pos + 1, line_pos - src_pos - 1);

          // classify the line spec
          spec_type = function_file_and_line;
          if (spec[line_pos] == '+')
            lineno_type = RELATIVE;
          else if (spec[line_pos + 1] == '*' &&
                   spec.length() == line_pos + 2)
            lineno_type = WILDCARD;
          else
            lineno_type = ABSOLUTE;

          if (lineno_type != WILDCARD)
            try
              {
                // try to parse N, N-M, or N,M,O,P, or combination thereof...
                if (spec.find_first_of(",-", line_pos + 1) != string::npos)
                  {
                    lineno_type = ENUMERATED;
                    vector<string> sub_specs;
                    tokenize(spec.substr(line_pos + 1), sub_specs, ",");
                    vector<string>::const_iterator line_spec;
                    for (line_spec = sub_specs.begin(); line_spec != sub_specs.end(); ++line_spec)
                      {
                        vector<string> ranges;
                        tokenize(*line_spec, ranges, "-");
                        if (ranges.size() > 1)
                          {
                            int low = lex_cast<int>(ranges.front());
                            int high = lex_cast<int>(ranges.back());
                            for (int i = low; i <= high; i++)
                                linenos.push_back(i);
                          }
                        else
                            linenos.push_back(lex_cast<int>(ranges.at(0)));
                      }
                    sort(linenos.begin(), linenos.end());
                  }
                else
                  {
                    linenos.push_back(lex_cast<int>(spec.substr(line_pos + 1)));
                    linenos.push_back(lex_cast<int>(spec.substr(line_pos + 1)));
                  }
              }
            catch (runtime_error & exn)
              {
                goto bad;
              }
        }
    }

  if (function.empty() ||
      (spec_type != function_alone && file.empty()))
    goto bad;

  if (sess.verbose > 2)
    {
      //clog << "parsed '" << spec << "'";
      clog << _F("parse '%s'", spec.c_str());

      if (!scopes.empty())
        clog << ", scope '" << scopes[0] << "'";
      for (unsigned i = 1; i < scopes.size(); ++i)
        clog << "::'" << scopes[i] << "'";

      clog << ", func '" << function << "'";

      if (spec_type != function_alone)
        clog << ", file '" << file << "'";

      if (spec_type == function_file_and_line)
        {
          clog << ", line ";
          switch (lineno_type)
            {
            case ABSOLUTE:
              clog << linenos[0];
              break;

            case RELATIVE:
              clog << "+" << linenos[0];
              break;

            case ENUMERATED:
              {
                vector<int>::const_iterator linenos_it;
                for (linenos_it = linenos.begin(); linenos_it != linenos.end(); ++linenos_it)
                  {
                    vector<int>::const_iterator range_it(linenos_it);
                    while ((range_it+1) != linenos.end() && *range_it + 1 == *(range_it+1))
                        ++range_it;
                    if (linenos_it == range_it)
                        clog << *linenos_it;
                    else
                        clog << *linenos_it << "-" << *range_it;
                    if (range_it + 1 != linenos.end())
                      clog << ",";
                    linenos_it = range_it;
                  }
                }
              break;

            case WILDCARD:
              clog << "*";
              break;
            }
        }

      clog << endl;
    }

  return;

bad:
  throw SEMANTIC_ERROR(_F("malformed specification '%s'", spec.c_str()),
                       base_probe->tok);
}

string path_remove_sysroot(const systemtap_session& sess, const string& path)
{
  size_t pos;
  string retval = path;
  if (!sess.sysroot.empty() &&
      (pos = retval.find(sess.sysroot)) != string::npos)
    retval.replace(pos, sess.sysroot.length(), "/");
  return retval;
}

void
dwarf_query::add_probe_point(const string& dw_funcname,
			     const char* filename,
			     int line,
			     Dwarf_Die* scope_die,
			     Dwarf_Addr addr)
{
  string reloc_section; // base section for relocation purposes
  Dwarf_Addr reloc_addr; // relocated
  const string& module = dw.module_name; // "kernel" or other
  string funcname = dw_funcname;

  assert (! has_absolute); // already handled in dwarf_builder::build()

  reloc_addr = dw.relocate_address(addr, reloc_section);

  // If we originally used the linkage name, then let's call it that way
  const char* linkage_name;
  if (!null_die(scope_die) && startswith (this->function, "_Z")
      && (linkage_name = dwarf_linkage_name (scope_die)))
    funcname = linkage_name;

  if (sess.verbose > 1)
    {
      clog << _("probe ") << funcname << "@" << filename << ":" << line;
      if (string(module) == TOK_KERNEL)
        clog << _(" kernel");
      else if (has_module)
        clog << _(" module=") << module;
      else if (has_process)
        clog << _(" process=") << module;
      if (reloc_section != "") clog << " reloc=" << reloc_section;
      clog << " pc=0x" << hex << addr << dec;
    }

  dwflpp::blacklisted_type blacklisted = dw.blacklisted_p (funcname, filename,
                                                           line, module, addr,
                                                           has_return);
  if (sess.verbose > 1)
    clog << endl;

  if (module == TOK_KERNEL)
    {
      // PR 4224: adapt to relocatable kernel by subtracting the _stext address here.
      reloc_addr = addr - sess.sym_stext;
      reloc_section = "_stext"; // a message to runtime's _stp_module_relocate
    }

  if (!blacklisted)
    {
      sess.unwindsym_modules.insert (module);

      if (has_process)
        {
          string module_tgt = path_remove_sysroot(sess, module);
          results.push_back (new uprobe_derived_probe(funcname, filename, line,
                                                      module_tgt, reloc_section, addr, reloc_addr,
                                                      *this, scope_die));
        }
      else
        {
          assert (has_kernel || has_module);
          results.push_back (new dwarf_derived_probe(funcname, filename, line,
                                                     module, reloc_section, addr, reloc_addr,
                                                     *this, scope_die));
        }
    }
  else
    {
      switch (blacklisted)
        {
        case dwflpp::blacklisted_section:
          sess.print_warning(_F("function %s is in blacklisted section",
                                funcname.c_str()), base_probe->tok);
          break;
        case dwflpp::blacklisted_kprobes:
          sess.print_warning(_F("kprobes function %s is blacklisted",
                                funcname.c_str()), base_probe->tok);
          break;
        case dwflpp::blacklisted_function_return:
          sess.print_warning(_F("function %s return probe is blacklisted",
                                funcname.c_str()), base_probe->tok);
          break;
        case dwflpp::blacklisted_file:
          sess.print_warning(_F("function %s is in blacklisted file",
                                funcname.c_str()), base_probe->tok);
          break;
        case dwflpp::blacklisted_function:
        default:
          sess.print_warning(_F("function %s is blacklisted",
                                funcname.c_str()), base_probe->tok);
          break;
        }
    }
}

void
dwarf_query::mount_well_formed_probe_point()
{
  string module = dw.module_name;
  if (has_process)
    module = path_remove_sysroot(sess, module);

  vector<probe_point::component*> comps;
  vector<probe_point::component*>::iterator it;
  for (it  = base_loc->components.begin();
       it != base_loc->components.end(); ++it)
    {
      if ((*it)->functor == TOK_PROCESS || (*it)->functor == TOK_MODULE)
        comps.push_back(new probe_point::component((*it)->functor,
          new literal_string(has_library ? path : module)));
      else
        comps.push_back(*it);
    }

  probe_point *pp = new probe_point(*base_loc);
  pp->well_formed = true;
  pp->components = comps;

  previous_bases.push(make_pair(base_loc, base_probe));

  base_loc = pp;
  base_probe = new probe(base_probe, pp);
}

void
dwarf_query::unmount_well_formed_probe_point()
{
  assert(!previous_bases.empty());

  base_loc = previous_bases.top().first;
  base_probe = previous_bases.top().second;

  previous_bases.pop();
}

void
dwarf_query::replace_probe_point_component_arg(const string& functor,
                                               const string& new_functor,
                                               int64_t new_arg,
                                               bool hex)
{
  // only allow these operations if we're editing the well-formed loc
  assert(!previous_bases.empty());

  vector<probe_point::component*>::iterator it;
  for (it  = base_loc->components.begin();
       it != base_loc->components.end(); ++it)
    if ((*it)->functor == functor)
      *it = new probe_point::component(new_functor,
              new literal_number(new_arg, hex));
}

void
dwarf_query::replace_probe_point_component_arg(const string& functor,
                                               int64_t new_arg,
                                               bool hex)
{
  replace_probe_point_component_arg(functor, functor, new_arg, hex);
}

void
dwarf_query::replace_probe_point_component_arg(const string& functor,
                                               const string& new_functor,
                                               const string& new_arg)
{
  // only allow these operations if we're editing the well-formed loc
  assert(!previous_bases.empty());

  vector<probe_point::component*>::iterator it;
  for (it  = base_loc->components.begin();
       it != base_loc->components.end(); ++it)
    if ((*it)->functor == functor)
      *it = new probe_point::component(new_functor,
              new literal_string(new_arg));
}

void
dwarf_query::replace_probe_point_component_arg(const string& functor,
                                               const string& new_arg)
{
  replace_probe_point_component_arg(functor, functor, new_arg);
}

void
dwarf_query::remove_probe_point_component(const string& functor)
{
  // only allow these operations if we're editing the well-formed loc
  assert(!previous_bases.empty());

  vector<probe_point::component*> new_comps;
  vector<probe_point::component*>::iterator it;
  for (it  = base_loc->components.begin();
       it != base_loc->components.end(); ++it)
    if ((*it)->functor != functor)
      new_comps.push_back(*it);

  base_loc->components = new_comps;
}

enum dbinfo_reqt
dwarf_query::assess_dbinfo_reqt()
{
  if (has_absolute)
    {
      // kernel.statement(NUM).absolute
      return dbr_none;
    }
  if (has_inline || has_label || has_callee || has_callees_num)
    {
      // kernel.function("f").inline or module("m").function("f").inline
      return dbr_need_dwarf;
    }
  if (has_function_str && spec_type == function_alone)
    {
      // kernel.function("f") or module("m").function("f")
      return dbr_need_symtab;
    }
  if (has_statement_num)
    {
      // kernel.statement(NUM) or module("m").statement(NUM)
      // Technically, all we need is the module offset (or _stext, for
      // the kernel).  But for that we need either the ELF file or (for
      // _stext) the symbol table.  In either case, the symbol table
      // is available, and that allows us to map the NUM (address)
      // to a function, which is goodness.
      return dbr_need_symtab;
    }
  if (has_function_num)
    {
      // kernel.function(NUM) or module("m").function(NUM)
      // Need the symbol table so we can back up from NUM to the
      // start of the function.
      return dbr_need_symtab;
    }
  // Symbol table tells us nothing about source files or line numbers.
  return dbr_need_dwarf;
}

string
dwarf_query::final_function_name(const string& final_func,
                                 const char* final_file,
                                 int final_line)
{
  string final_name = final_func;
  if (final_file && *final_file != '\0')
    {
      final_name += ("@" + string(final_file));
      if (final_line > 0)
        final_name += (":" + lex_cast(final_line));
    }
  return final_name;
}

bool
dwarf_query::is_fully_specified_function()
{
  // A fully specified function is one that was given using a .function() probe
  // by full name (no wildcards), and specific srcfile and decl_line.
  return (has_function_str
          && spec_type == function_file_and_line
          && !dw.name_has_wildcard(function)
          && filtered_srcfiles.size() == 1
          && !filtered_functions.empty()
          && lineno_type == ABSOLUTE
          && filtered_functions[0].decl_line == linenos[0]);
}

base_func_info_map_t
dwarf_query::filtered_all(void)
{
  base_func_info_map_t r;
  func_info_map_t::const_iterator f;
  for (f  = filtered_functions.begin();
       f != filtered_functions.end(); ++f)
    r.push_back(*f);
  inline_instance_map_t::const_iterator i;
  for (i  = filtered_inlines.begin();
       i != filtered_inlines.end(); ++i)
    r.push_back(*i);
  return r;
}

// The critical determining factor when interpreting a pattern
// string is, perhaps surprisingly: "presence of a lineno". The
// presence of a lineno changes the search strategy completely.
//
// Compare the two cases:
//
//   1. {statement,function}(foo@file.c:lineno)
//      - find the files matching file.c
//      - in each file, find the functions matching foo
//      - query the file for line records matching lineno
//      - iterate over the line records,
//        - and iterate over the functions,
//          - if(haspc(function.DIE, line.addr))
//            - if looking for statements: probe(lineno.addr)
//            - if looking for functions: probe(function.{entrypc,return,etc.})
//
//   2. {statement,function}(foo@file.c)
//      - find the files matching file.c
//      - in each file, find the functions matching foo
//        - probe(function.{entrypc,return,etc.})
//
// Thus the first decision we make is based on the presence of a
// lineno, and we enter entirely different sets of callbacks
// depending on that decision.
//
// Note that the first case is a generalization fo the second, in that
// we could theoretically search through line records for matching
// file names (a "table scan" in rdbms lingo).  Luckily, file names
// are already cached elsewhere, so we can do an "index scan" as an
// optimization.

static void
query_statement (string const & func,
		 char const * file,
		 int line,
		 Dwarf_Die *scope_die,
		 Dwarf_Addr stmt_addr,
		 dwarf_query * q)
{
  if (1)
  {
    if (q->dw.is_mips64_msym32(scope_die))
      {
        /* Force sign extension */
        if (q->sess.verbose > 2)
          clog << "query_statement " << func << "@" << file << ":" << line
            << " sign-extend stmt_addr " << hex << stmt_addr
            << " to " << hex << ((int64_t)((int32_t)stmt_addr))
            << " because were on mips64 with 32bit symbols" << endl;
        stmt_addr = (int32_t)stmt_addr;
      }
  }

  try
    {
      q->add_probe_point(func, file ? file : "",
                         line, scope_die, stmt_addr);
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
    }
}

static void
query_addr(Dwarf_Addr addr, dwarf_query *q)
{
  assert(q->has_function_num || q->has_statement_num);

  dwflpp &dw = q->dw;

  if (q->sess.verbose > 2)
    clog << "query_addr 0x" << hex << addr << dec << endl;

  // First pick which CU contains this address
  Dwarf_Die* cudie = dw.query_cu_containing_address(addr);
  if (!cudie) // address could be wildly out of range
    return;
  dw.focus_on_cu(cudie);

  // Now compensate for the dw bias
  addr -= dw.module_bias;

  // Per PR5787, we look up the scope die even for
  // statement_num's, for blacklist sensitivity and $var
  // resolution purposes.

  // Find the scopes containing this address
  vector<Dwarf_Die> scopes = dw.getscopes(addr);
  if (scopes.empty())
    return;

  // Look for the innermost containing function
  Dwarf_Die *fnscope = NULL;
  for (size_t i = 0; i < scopes.size(); ++i)
    {
      int tag = dwarf_tag(&scopes[i]);
      if ((tag == DW_TAG_subprogram && !q->has_inline) ||
          (tag == DW_TAG_inlined_subroutine &&
           !q->has_call && !q->has_return && !q->has_exported))
        {
          fnscope = &scopes[i];
          break;
        }
    }
  if (!fnscope)
    return;
  dw.focus_on_function(fnscope);

  Dwarf_Die *scope = q->has_function_num ? fnscope : &scopes[0];

  const char *file = dwarf_decl_file(fnscope);
  int line;
  dwarf_decl_line(fnscope, &line);

  // Function probes should reset the addr to the function entry
  // and possibly perform prologue searching
  if (q->has_function_num)
    {
      if (!dw.die_entrypc(fnscope, &addr))
        return;
      if (dwarf_tag(fnscope) == DW_TAG_subprogram &&
          (q->sess.prologue_searching ||
           (q->has_process && !q->dw.has_valid_locs()))) // PR 6871 && PR 6941
        {
          func_info func;
          func.die = *fnscope;
          func.name = dw.function_name;
          func.decl_file = file;
          func.decl_line = line;
          func.entrypc = addr;

          func_info_map_t funcs(1, func);
          dw.resolve_prologue_endings (funcs);
          q->prologue_end = funcs[0].prologue_end;

          // PR13200: if it's a .return probe, we need to emit a *retprobe based
          // on the entrypc so here we only use prologue_end for non .return
          // probes (note however that .return probes still take advantage of
          // prologue_end: PR14436)
          if (!q->has_return)
            addr = funcs[0].prologue_end;
        }
    }
  else
    {
      Dwarf_Line *address_line = dwarf_getsrc_die(cudie, addr);
      Dwarf_Addr address_line_addr = addr;
      if (address_line)
        {
          file = DWARF_LINESRC(address_line);
          line = DWARF_LINENO(address_line);
          address_line_addr = DWARF_LINEADDR(address_line);
        }

      // Verify that a raw address matches the beginning of a
      // statement. This is a somewhat lame check that the address
      // is at the start of an assembly instruction.  Mark probes are in the
      // middle of a macro and thus not strictly at a statement beginning.
      // Guru mode may override this check.
      if (!q->has_mark && (!address_line || address_line_addr != addr))
        {
          stringstream msg;
          msg << _F("address %#" PRIx64 " does not match the beginning of a statement",
                    addr);
          if (address_line)
            msg << _F(" (try %#" PRIx64 ")", address_line_addr);
          else
            msg << _F(" (no line info found for '%s', in module '%s')",
                      dw.cu_name().c_str(), dw.module_name.c_str());
          if (! q->sess.guru_mode)
            throw SEMANTIC_ERROR(msg.str());
          else
           q->sess.print_warning(msg.str());
        }
    }

  // We're ready to build a probe, but before, we need to create the final,
  // well-formed version of this location with all the components filled in
  q->mount_well_formed_probe_point();
  q->replace_probe_point_component_arg(TOK_FUNCTION, addr, true /* hex */ );
  q->replace_probe_point_component_arg(TOK_STATEMENT, addr, true /* hex */ );

  // Build a probe at this point
  query_statement(dw.function_name, file, line, scope, addr, q);

  q->unmount_well_formed_probe_point();
}

static void
query_plt_statement(dwarf_query *q)
{
  assert (q->has_plt && q->has_statement_num);

  Dwarf_Addr addr = q->statement_num_val;
  if (q->sess.verbose > 2)
    clog << "query_plt_statement 0x" << hex << addr << dec << endl;

  // First adjust the raw address to dwfl's elf bias.
  Dwarf_Addr elf_bias;
  Elf *elf = dwfl_module_getelf (q->dw.module, &elf_bias);
  assert(elf);
  addr += elf_bias;

  // Now compensate for the dw bias
  q->dw.get_module_dwarf(false, false);
  addr -= q->dw.module_bias;

  // Create the final well-formed probe point
  q->mount_well_formed_probe_point();
  q->replace_probe_point_component_arg(TOK_STATEMENT, q->statement_num_val, true /* hex */ );

  // We remove the .plt part here, since if the user provided a .plt probe, then
  // the higher-level probe point is already well-formed. On the other hand, if
  // the user provides a .plt(PATTERN).statement(0xABCD), the PATTERN is
  // irrelevant (we won't iterate over plts) so just take it out.
  q->remove_probe_point_component(TOK_PLT);

  // Build a probe at this point
  query_statement(q->plt_val, NULL, -1, NULL, addr, q);

  q->unmount_well_formed_probe_point();
}

static void
query_label (const base_func_info& func,
             char const * label,
             char const * file,
             int line,
             Dwarf_Die *scope_die,
             Dwarf_Addr stmt_addr,
             dwarf_query * q)
{
  assert (q->has_statement_str || q->has_function_str);

  // weed out functions whose decl_file isn't one of
  // the source files that we actually care about
  if (q->spec_type != function_alone &&
      q->filtered_srcfiles.count(file) == 0)
    return;

  // Create the final well-formed probe
  string canon_func = q->final_function_name(func.name, file, line);

  q->mount_well_formed_probe_point();
  q->replace_probe_point_component_arg(TOK_FUNCTION, canon_func);
  q->replace_probe_point_component_arg(TOK_LABEL, label);

  query_statement(func.name, file, line, scope_die, stmt_addr, q);

  q->unmount_well_formed_probe_point();
}

static void
query_callee (base_func_info& callee,
              base_func_info& caller,
              stack<Dwarf_Addr> *callers,
              dwarf_query * q)
{
  assert (q->has_function_str);
  assert (q->has_callee || q->has_callees_num);

  // OK, we found a callee for a targeted caller. To help users see the
  // derivation, we add the well-formed form .function(caller).callee(callee).

  string canon_caller = q->final_function_name(caller.name, caller.decl_file,
                                               caller.decl_line);
  string canon_callee = q->final_function_name(callee.name, callee.decl_file,
                                               callee.decl_line);

  q->mount_well_formed_probe_point();
  q->replace_probe_point_component_arg(TOK_FUNCTION, canon_caller);
  q->replace_probe_point_component_arg(TOK_CALLEES, TOK_CALLEE, canon_callee);
  q->replace_probe_point_component_arg(TOK_CALLEE, canon_callee);

  // Pass on the callers we'll need to add checks for
  q->callers = callers;

  query_statement(callee.name, callee.decl_file, callee.decl_line,
                  &callee.die, callee.entrypc, q);

  q->unmount_well_formed_probe_point();
}

static void
query_inline_instance_info (inline_instance_info & ii,
			    dwarf_query * q)
{
  try
    {
      assert (! q->has_return); // checked by caller already
      assert (q->has_function_str || q->has_statement_str);

      if (q->sess.verbose>2)
        clog << _F("querying entrypc %#" PRIx64 " of instance of inline '%s'\n",
                   ii.entrypc, ii.name.c_str());

      string canon_func = q->final_function_name(ii.name, ii.decl_file,
                                                 ii.decl_line);

      q->mount_well_formed_probe_point();
      q->replace_probe_point_component_arg(TOK_FUNCTION, canon_func);
      q->replace_probe_point_component_arg(TOK_STATEMENT, canon_func);

      query_statement (ii.name, ii.decl_file, ii.decl_line,
                       &ii.die, ii.entrypc, q);

      q->unmount_well_formed_probe_point();
    }
  catch (semantic_error &e)
    {
      q->sess.print_error (e);
    }
}

static void
query_func_info (Dwarf_Addr entrypc,
		 func_info & fi,
		 dwarf_query * q)
{
  assert(q->has_function_str || q->has_statement_str);

  try
    {
      string canon_func = q->final_function_name(fi.name, fi.decl_file,
                                                 fi.decl_line);

      q->mount_well_formed_probe_point();
      q->replace_probe_point_component_arg(TOK_FUNCTION, canon_func);
      q->replace_probe_point_component_arg(TOK_STATEMENT, canon_func);

      // If it's a .return probe, we need to emit a *retprobe based on the
      // entrypc (PR13200). Note however that if prologue_end is valid,
      // dwarf_derived_probe will still take advantage of it by creating a new
      // probe there if necessary to pick up target vars (PR14436).
      if (fi.prologue_end == 0 || q->has_return)
        {
          q->prologue_end = fi.prologue_end;
          query_statement (fi.name, fi.decl_file, fi.decl_line,
                           &fi.die, entrypc, q);
        }
      else
        {
          query_statement (fi.name, fi.decl_file, fi.decl_line,
                           &fi.die, fi.prologue_end, q);
        }

      q->unmount_well_formed_probe_point();
    }
  catch (semantic_error &e)
    {
      q->sess.print_error (e);
    }
}

static void
query_srcfile_line (Dwarf_Addr addr, int lineno, dwarf_query * q)
{
  assert (q->has_statement_str || q->has_function_str);
  assert (q->spec_type == function_file_and_line);

  base_func_info_map_t bfis = q->filtered_all();
  base_func_info_map_t::iterator i;
  for (i = bfis.begin(); i != bfis.end(); ++i)
    {
      if (q->dw.die_has_pc (i->die, addr))
        {
          if (q->sess.verbose>3)
            clog << _("filtered DIE lands on srcfile\n");
          Dwarf_Die scope;
          q->dw.inner_die_containing_pc(i->die, addr, scope);

          string canon_func = q->final_function_name(i->name, i->decl_file,
                                                     lineno /* NB: not i->decl_line */ );

          if (q->has_nearest && (q->lineno_type == ABSOLUTE ||
                                 q->lineno_type == RELATIVE))
            {
              int lineno_nearest = q->linenos[0];
              if (q->lineno_type == RELATIVE)
                lineno_nearest += i->decl_line;
              string canon_func_nearest = q->final_function_name(i->name, i->decl_file,
                                                                 lineno_nearest);
              q->mount_well_formed_probe_point();
              q->replace_probe_point_component_arg(TOK_STATEMENT, canon_func_nearest);
            }

          q->mount_well_formed_probe_point();
          q->replace_probe_point_component_arg(TOK_FUNCTION, canon_func);
          q->replace_probe_point_component_arg(TOK_STATEMENT, canon_func);

          query_statement (i->name, i->decl_file,
                           lineno, // NB: not q->line !
                           &scope, addr, q);

          q->unmount_well_formed_probe_point();
          if (q->has_nearest && (q->lineno_type == ABSOLUTE ||
                                 q->lineno_type == RELATIVE))
            q->unmount_well_formed_probe_point();
        }
    }
}

bool
inline_instance_info::operator<(const inline_instance_info& other) const
{
  if (entrypc != other.entrypc)
    return entrypc < other.entrypc;

  if (decl_line != other.decl_line)
    return decl_line < other.decl_line;

  int cmp = name.compare(other.name);

  if (!cmp) 
    {
      assert (decl_file);
      assert (other.decl_file);
      cmp = strcmp(decl_file, other.decl_file);
    }

  return cmp < 0;
}


static int
query_dwarf_inline_instance (Dwarf_Die * die, dwarf_query * q)
{
  assert (q->has_statement_str || q->has_function_str);
  assert (!q->has_call && !q->has_return && !q->has_exported);

  try
    {
      if (q->sess.verbose>2)
        clog << _F("selected inline instance of %s\n", q->dw.function_name.c_str());

      Dwarf_Addr entrypc;
      if (q->dw.die_entrypc (die, &entrypc))
        {
          inline_instance_info inl;
          inl.die = *die;
          inl.name = q->dw.function_name;
          inl.entrypc = entrypc;
          q->dw.function_file (&inl.decl_file);
          q->dw.function_line (&inl.decl_line);

          // make sure that this inline hasn't already
          // been matched from a different CU
          if (q->inline_dupes.insert(inl).second)
            q->filtered_inlines.push_back(inl);
        }
      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}

static bool
is_filtered_func_exists (func_info_map_t const& filtered, func_info *fi)
{
  for (unsigned i = 0; i < filtered.size(); i++)
    {
      if ((filtered[i].entrypc == fi->entrypc) && (filtered[i].name == fi->name))
        return true;
    }

  return false;
}

static int
query_dwarf_func (Dwarf_Die * func, dwarf_query * q)
{
  assert (q->has_statement_str || q->has_function_str);

  // weed out functions whose decl_file isn't one of
  // the source files that we actually care about
  if (q->spec_type != function_alone &&
      q->filtered_srcfiles.count(dwarf_decl_file(func)?:"") == 0)
    return DWARF_CB_OK;

  try
    {
      q->dw.focus_on_function (func);

      if (!q->dw.function_scope_matches(q->scopes))
        return DWARF_CB_OK;

      // make sure that this function address hasn't
      // already been matched under an aliased name
      Dwarf_Addr addr;
      if (!q->dw.func_is_inline() &&
          dwarf_entrypc(func, &addr) == 0 &&
          !q->alias_dupes.insert(addr).second)
        return DWARF_CB_OK;

      if (q->dw.func_is_inline () && (! q->has_call) && (! q->has_return) && (! q->has_exported))
	{
          if (q->sess.verbose>3)
            clog << _F("checking instances of inline %s\n", q->dw.function_name.c_str());
          q->dw.iterate_over_inline_instances (query_dwarf_inline_instance, q);
	}
      else if (q->dw.func_is_inline () && (q->has_return)) // PR 11553
	{
          q->inlined_non_returnable.insert (q->dw.function_name);
	}
      else if (!q->dw.func_is_inline () && (! q->has_inline))
	{
          if (q->has_exported && !q->dw.func_is_exported ())
            return DWARF_CB_OK;
          if (q->sess.verbose>2)
            clog << _F("selected function %s\n", q->dw.function_name.c_str());

          func_info func;
          q->dw.function_die (&func.die);
          func.name = q->dw.function_name;
          q->dw.function_file (&func.decl_file);
          q->dw.function_line (&func.decl_line);

          Dwarf_Addr entrypc;

          func.entrypc = 0;
          Dwarf_Addr bias;
          Dwfl_Module *mod = q->dw.module;
          Elf* elf = (dwarf_getelf (dwfl_module_getdwarf (mod, &bias))
                     ?: dwfl_module_getelf (mod, &bias));

          GElf_Ehdr ehdr_mem;
          GElf_Ehdr* em = gelf_getehdr (elf, &ehdr_mem);
          if (em == NULL) throw SEMANTIC_ERROR (_("Couldn't get elf header"));

          /* Giving priority to sym_table for ppc64*/
          if ((em->e_machine == EM_PPC64) && ((em->e_flags & EF_PPC64_ABI) == 2)
              && (q->dw.mod_info->sym_table))
            {
              /* The linkage name is the best match for the symbol table. */
              const string& linkage_name = dwarf_linkage_name(&func.die)
                ?: dwarf_diename(&func.die) ?: func.name;

              set<func_info *> fis = q->dw.mod_info->sym_table->lookup_symbol(linkage_name);
              for (set<func_info*>::iterator it=fis.begin(); it!=fis.end() ; ++it)
                {
                  func.entrypc = (*it)->entrypc;
                  if (is_filtered_func_exists(q->filtered_functions, &func))
                    continue;
                  q->filtered_functions.push_back(func);
                }
            }

          /* If not ppc64 or not found in sym_table, try it directly. */
          if (!func.entrypc && q->dw.function_entrypc (&entrypc))
            {
              func.entrypc = entrypc;
              q->filtered_functions.push_back (func);
            }
          /* else this function is fully inlined, just ignore it */
	}
      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}

static int
query_cu (Dwarf_Die * cudie, dwarf_query * q)
{
  assert (q->has_statement_str || q->has_function_str);

  if (pending_interrupts) return DWARF_CB_ABORT;

  try
    {
      q->dw.focus_on_cu (cudie);

      if (false && q->sess.verbose>2)
        clog << _F("focused on CU '%s', in module '%s'\n",
                   q->dw.cu_name().c_str(), q->dw.module_name.c_str());

      q->filtered_srcfiles.clear();
      q->filtered_functions.clear();
      q->filtered_inlines.clear();

      // In this path, we find "abstract functions", record
      // information about them, and then (depending on lineno
      // matching) possibly emit one or more of the function's
      // associated addresses. Unfortunately the control of this
      // cannot easily be turned inside out.

      if (q->spec_type != function_alone)
        {
          // If we have a pattern string with a filename, we need
          // to elaborate the srcfile mask in question first.
          q->dw.collect_srcfiles_matching (q->file, q->filtered_srcfiles);

          // If we have a file pattern and *no* srcfile matches, there's
          // no need to look further into this CU, so skip.
          if (q->filtered_srcfiles.empty())
            return DWARF_CB_OK;
        }

      // Pick up [entrypc, name, DIE] tuples for all the functions
      // matching the query, and fill in the prologue endings of them
      // all in a single pass.
      int rc = q->dw.iterate_over_functions (query_dwarf_func, q, q->function);
      if (rc != DWARF_CB_OK)
        q->query_done = true;

      if (!q->filtered_functions.empty() &&
          !q->has_statement_str && // PR 2608
           (q->sess.prologue_searching ||
            (q->has_process && !q->dw.has_valid_locs()))) // PR 6871 && PR 6941
        q->dw.resolve_prologue_endings (q->filtered_functions);

      if (q->has_label)
        {
          enum lineno_t lineno_type = WILDCARD;
          if (q->spec_type == function_file_and_line)
            lineno_type = q->lineno_type;
          base_func_info_map_t bfis = q->filtered_all();
          base_func_info_map_t::iterator i;
          for (i = bfis.begin(); i != bfis.end(); ++i)
            q->dw.iterate_over_labels (&i->die, q->label_val, *i, q->linenos,
                                       lineno_type, q, query_label);
        }
      else if (q->has_callee || q->has_callees_num)
        {
          // .callee(str) --> str, .callees[(N)] --> "*"
          string callee_val = q->has_callee ? q->callee_val : "*";
          long callees_num_val = q->has_callees_num ? q->callees_num_val : 1;

          // NB: We filter functions that do not match the file here rather than
          // in query_callee because we only want the filtering to apply to the
          // first level, not to callees that are recursed into if
          // callees_num_val > 1.
          base_func_info_map_t bfis = q->filtered_all();
          base_func_info_map_t::iterator i;
          for (i = bfis.begin(); i != bfis.end(); ++i)
            {
              if (q->spec_type != function_alone &&
                  q->filtered_srcfiles.count(i->decl_file) == 0)
                continue;
              q->dw.iterate_over_callees (&i->die, callee_val,
                                          callees_num_val,
                                          q, query_callee, *i);
            }
        }
      else if (q->spec_type == function_file_and_line
              // User specified function, file and lineno, but if they match
              // exactly a specific function in a specific line at a specific
              // decl_line, the user doesn't actually want to probe a lineno,
              // but rather the function itself. So let fall through to
              // query_func_info/query_inline_instance_info in final else.
               && !q->is_fully_specified_function())
        {
          // .statement(...:NN) often gets mixed up with .function(...:NN)
          if (q->has_function_str)
            q->sess.print_warning (_("For probing a particular line, use a "
                                   ".statement() probe, not .function()"),
                                   q->base_probe->tok);

          base_func_info_map_t bfis = q->filtered_all();

          set<string>::const_iterator srcfile;
          for (srcfile  = q->filtered_srcfiles.begin();
               srcfile != q->filtered_srcfiles.end(); ++srcfile)
            q->dw.iterate_over_srcfile_lines(srcfile->c_str(), q->linenos,
                                             q->lineno_type, bfis,
                                             query_srcfile_line,
                                             q->has_nearest, q);
        }
      else
        {
          // Otherwise, simply probe all resolved functions.
          for (func_info_map_t::iterator i = q->filtered_functions.begin();
               i != q->filtered_functions.end(); ++i)
            query_func_info (i->entrypc, *i, q);

          // And all inline instances (if we're not excluding inlines with ".call")
          if (! q->has_call)
            for (inline_instance_map_t::iterator i
                   = q->filtered_inlines.begin(); i != q->filtered_inlines.end(); ++i)
              query_inline_instance_info (*i, q);
        }
      return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}


void
dwarf_query::query_module_functions ()
{
  try
    {
      filtered_srcfiles.clear();
      filtered_functions.clear();
      filtered_inlines.clear();

      // Collect all module functions so we know which CUs are interesting
      int rc = dw.iterate_single_function(query_dwarf_func, this, function);
      if (rc != DWARF_CB_OK)
        {
          query_done = true;
          return;
        }

      set<void*> used_cus; // by cu->addr
      vector<Dwarf_Die> cus;
      Dwarf_Die cu_mem;

      base_func_info_map_t bfis = filtered_all();
      base_func_info_map_t::iterator i;
      for (i = bfis.begin(); i != bfis.end(); ++i)
        if (dwarf_diecu(&i->die, &cu_mem, NULL, NULL) &&
            used_cus.insert(cu_mem.addr).second)
          cus.push_back(cu_mem);

      // Reset the dupes since we didn't actually collect them the first time
      alias_dupes.clear();
      inline_dupes.clear();

      // Run the query again on the individual CUs
      for (vector<Dwarf_Die>::iterator i = cus.begin(); i != cus.end(); ++i){
        rc = query_cu(&*i, this);
	if (rc != DWARF_CB_OK)
	  {
	    query_done = true;
	    return;
	  }
      }
    }
  catch (const semantic_error& e)
    {
      sess.print_error (e);
    }
}


static void
validate_module_elf (Dwfl_Module *mod, const char *name,  base_query *q)
{
  // Validate the machine code in this elf file against the
  // session machine.  This is important, in case the wrong kind
  // of debuginfo is being automagically processed by elfutils.
  // While we can tell i686 apart from x86-64, unfortunately
  // we can't help confusing i586 vs i686 (both EM_386).

  Dwarf_Addr bias;
  // We prefer dwfl_module_getdwarf to dwfl_module_getelf here,
  // because dwfl_module_getelf can force costly section relocations
  // we don't really need, while either will do for this purpose.
  Elf* elf = (dwarf_getelf (dwfl_module_getdwarf (mod, &bias))
		  ?: dwfl_module_getelf (mod, &bias));

  GElf_Ehdr ehdr_mem;
  GElf_Ehdr* em = gelf_getehdr (elf, &ehdr_mem);
  if (em == 0) { DWFL_ASSERT ("dwfl_getehdr", dwfl_errno()); }
  assert(em);
  int elf_machine = em->e_machine;
  const char* debug_filename = "";
  const char* main_filename = "";
  (void) dwfl_module_info (mod, NULL, NULL,
                               NULL, NULL, NULL,
                               & main_filename,
                               & debug_filename);
  const string& sess_machine = q->sess.architecture;

  string expect_machine; // to match sess.machine (i.e., kernel machine)
  string expect_machine2;

  // NB: See also the 'uname -m' squashing done in main.cxx.
  switch (elf_machine)
    {
      // x86 and ppc are bi-architecture; a 64-bit kernel
      // can normally run either 32-bit or 64-bit *userspace*.
    case EM_386:
      expect_machine = "i?86";
      if (! q->has_process) break; // 32-bit kernel/module
      /* FALLSTHROUGH */
    case EM_X86_64:
      expect_machine2 = "x86_64";
      break;
    case EM_PPC:
    case EM_PPC64:
      expect_machine = "powerpc";
      break;
    case EM_S390: expect_machine = "s390"; break;
    case EM_IA_64: expect_machine = "ia64"; break;
    case EM_ARM: expect_machine = "arm*"; break;
    case EM_AARCH64: expect_machine = "arm64"; break;
    case EM_MIPS: expect_machine = "mips"; break;
      // XXX: fill in some more of these
    default: expect_machine = "?"; break;
    }

  if (! debug_filename) debug_filename = main_filename;
  if (! debug_filename) debug_filename = name;

  if (fnmatch (expect_machine.c_str(), sess_machine.c_str(), 0) != 0 &&
      fnmatch (expect_machine2.c_str(), sess_machine.c_str(), 0) != 0)
    {
      stringstream msg;
      msg << _F("ELF machine %s|%s (code %d) mismatch with target %s in '%s'",
                expect_machine.c_str(), expect_machine2.c_str(), elf_machine,
                sess_machine.c_str(), debug_filename);
      throw SEMANTIC_ERROR(msg.str ());
    }

  if (q->sess.verbose>1)
    clog << _F("focused on module '%s' = [%#" PRIx64 "-%#" PRIx64 ", bias %#" PRIx64
               " file %s ELF machine %s|%s (code %d)\n",
               q->dw.module_name.c_str(), q->dw.module_start, q->dw.module_end,
               q->dw.module_bias, debug_filename, expect_machine.c_str(),
               expect_machine2.c_str(), elf_machine);
}



static Dwarf_Addr
lookup_symbol_address (Dwfl_Module *m, const char* wanted)
{
  int syments = dwfl_module_getsymtab(m);
  assert(syments);
  for (int i = 1; i < syments; ++i)
    {
      GElf_Sym sym;
      const char *name = dwfl_module_getsym(m, i, &sym, NULL);
      if (name != NULL && strcmp(name, wanted) == 0)
        return sym.st_value;
    }

  return 0;
}



static int
query_module (Dwfl_Module *mod,
              void **,
	      const char *name,
              Dwarf_Addr addr,
	      base_query *q)
{
  try
    {
      module_info* mi = q->sess.module_cache->cache[name];
      if (mi == 0)
        {
          mi = q->sess.module_cache->cache[name] = new module_info(name);

          mi->mod = mod;
          mi->addr = addr;

          const char* debug_filename = "";
          const char* main_filename = "";
          (void) dwfl_module_info (mod, NULL, NULL,
                                   NULL, NULL, NULL,
                                   & main_filename,
                                   & debug_filename);

          if (debug_filename || main_filename)
            {
              mi->elf_path = debug_filename ?: main_filename;
            }
          else if (name == TOK_KERNEL)
            {
              mi->dwarf_status = info_absent;
            }
        }
      // OK, enough of that module_info caching business.

      q->dw.focus_on_module(mod, mi);

      // If we have enough information in the pattern to skip a module and
      // the module does not match that information, return early.
      if (!q->dw.module_name_matches(q->module_val))
        return pending_interrupts ? DWARF_CB_ABORT : DWARF_CB_OK;

      // Don't allow module("*kernel*") type expressions to match the
      // elfutils module "kernel", which we refer to in the probe
      // point syntax exclusively as "kernel.*".
      if (q->dw.module_name == TOK_KERNEL && ! q->has_kernel)
        return pending_interrupts ? DWARF_CB_ABORT : DWARF_CB_OK;

      if (mod)
        validate_module_elf(mod, name, q);
      else
        assert(q->has_kernel);   // and no vmlinux to examine

      if (q->sess.verbose>2)
        cerr << _F("focused on module '%s'\n", q->dw.module_name.c_str());


      // Collect a few kernel addresses.  XXX: these belong better
      // to the sess.module_info["kernel"] struct.
      if (q->dw.module_name == TOK_KERNEL)
        {
          if (! q->sess.sym_kprobes_text_start)
            q->sess.sym_kprobes_text_start = lookup_symbol_address (mod, "__kprobes_text_start");
          if (! q->sess.sym_kprobes_text_end)
            q->sess.sym_kprobes_text_end = lookup_symbol_address (mod, "__kprobes_text_end");
          if (! q->sess.sym_stext)
            q->sess.sym_stext = lookup_symbol_address (mod, "_stext");
        }

      // If there is a .library component, then q->path will hold the path to
      // the executable if the library was fully resolved. If not (e.g. not
      // absolute, or globby), resort to iterate_over_libraries().
      if (q->has_library && q->path.empty())
        q->dw.iterate_over_libraries (&q->query_library_callback, q);
      // .plt is translated to .plt.statement(N).  We only want to iterate for the
      // .plt case
      else if (q->has_plt && ! q->has_statement)
        {
          q->dw.iterate_over_plt (q, &q->query_plt_callback);
          q->visited_modules.insert(name);
        }
      else
        {
          // search the module for matches of the probe point.
          q->handle_query_module();
          q->visited_modules.insert(name);
        }

      // If we know that there will be no more matches, abort early.
      if (q->dw.module_name_final_match(q->module_val) || pending_interrupts)
        return DWARF_CB_ABORT;
      else
        return DWARF_CB_OK;
    }
  catch (const semantic_error& e)
    {
      q->sess.print_error (e);
      return DWARF_CB_ABORT;
    }
}


void
base_query::query_library_callback (base_query *me, const char *data)
{
  me->query_library (data);
}


probe*
build_library_probe(dwflpp& dw,
                    const string& library,
                    probe *base_probe,
                    probe_point *base_loc)
{
  probe_point* specific_loc = new probe_point(*base_loc);
  vector<probe_point::component*> derived_comps;

  // Create new probe point for the matching library. This is what will be
  // shown in listing mode. Also replace the process(str) with the real
  // absolute path rather than keeping what the user typed in.
  vector<probe_point::component*>::iterator it;
  for (it = specific_loc->components.begin();
      it != specific_loc->components.end(); ++it)
    if ((*it)->functor == TOK_PROCESS)
      derived_comps.push_back(new probe_point::component(TOK_PROCESS,
          new literal_string(path_remove_sysroot(dw.sess, dw.module_name))));
    else if ((*it)->functor == TOK_LIBRARY)
      derived_comps.push_back(new probe_point::component(TOK_LIBRARY,
          new literal_string(path_remove_sysroot(dw.sess, library)),
          true /* from_glob */ ));
    else
      derived_comps.push_back(*it);
  probe_point* derived_loc = new probe_point(*specific_loc);
  derived_loc->components = derived_comps;
  return new probe (new probe (base_probe, specific_loc), derived_loc);
}

bool
query_one_library (const char *library, dwflpp & dw,
    const string user_lib, probe * base_probe, probe_point *base_loc,
    vector<derived_probe *> & results)
{
  if (dw.function_name_matches_pattern(library, "*" + user_lib))
    {
      string library_path = find_executable (library, "", dw.sess.sysenv,
                                             "LD_LIBRARY_PATH");
      probe *new_base = build_library_probe(dw, library_path,
                                            base_probe, base_loc);

      // We pass true for the optional parameter of derive_probes() here to
      // indicate that we don't mind if the probe doesn't resolve. This is
      // because users expect wildcarded probe points to only apply to a subset
      // of matching libraries, in the sense of "any", rather than "all", just
      // like module("*") and process("*"). See also dwarf_builder::build().
      derive_probes(dw.sess, new_base, results, true /* optional */ );

      if (dw.sess.verbose > 2)
        clog << _("module=") << library_path << endl;
      return true;
    }
  return false;
}


void
dwarf_query::query_library (const char *library)
{
  visited_libraries.insert(library);
  if (query_one_library (library, dw, user_lib, base_probe, base_loc, results))
    resolved_library = true;
}

struct plt_expanding_visitor: public var_expanding_visitor
{
  plt_expanding_visitor(const string & entry):
    entry (entry)
  {
  }
  const string & entry;

  void visit_target_symbol (target_symbol* e);
};


void
base_query::query_plt_callback (base_query *me, const char *entry, size_t address)
{
  if (me->dw.function_name_matches_pattern (entry, me->plt_val))
    me->query_plt (entry, address);
  me->dw.mod_info->plt_funcs.insert(entry);
}


void
query_one_plt (const char *entry, long addr, dwflpp & dw,
    probe * base_probe, probe_point *base_loc,
    vector<derived_probe *> & results, base_query *q)
{
      string module = dw.module_name;
      if (q->has_process)
        module = path_remove_sysroot(dw.sess, module);

      probe_point* specific_loc = new probe_point(*base_loc);
      specific_loc->well_formed = true;

      vector<probe_point::component*> derived_comps;

      if (dw.sess.verbose > 2)
        clog << _F("plt entry=%s\n", entry);

      vector<probe_point::component*>::iterator it;
      for (it = specific_loc->components.begin();
          it != specific_loc->components.end(); ++it)
        if ((*it)->functor == TOK_PROCESS)
          {
            // Replace with fully resolved path
            *it = new probe_point::component(TOK_PROCESS,
                    new literal_string(q->has_library ? q->path : module));
            derived_comps.push_back(*it);
          }
        else if ((*it)->functor == TOK_PLT)
          {
            // Replace possibly globby component
            *it = new probe_point::component(TOK_PLT,
                                             new literal_string(entry));
            derived_comps.push_back(*it);
            derived_comps.push_back(new probe_point::component(TOK_STATEMENT,
                                                               new literal_number(addr, true)));
          }
        else
          derived_comps.push_back(*it);
      probe_point* derived_loc = new probe_point(*specific_loc);
      derived_loc->components = derived_comps;
      probe *new_base = new probe (new probe (base_probe, specific_loc),
                                   derived_loc);
      string e = string(entry);
      plt_expanding_visitor pltv (e);
      pltv.replace (new_base->body);

      literal_map_t params;
      for (unsigned i = 0; i < derived_loc->components.size(); ++i)
       {
          probe_point::component *c = derived_loc->components[i];
          params[c->functor] = c->arg;
       }
      dwarf_query derived_q(new_base, derived_loc, dw, params, results, "", "");
      dw.iterate_over_modules<base_query>(&query_module, &derived_q);
}


void
dwarf_query::query_plt (const char *entry, size_t address)
{
  query_one_plt (entry, address, dw, base_probe, base_loc, results, this);
}

// This would more naturally fit into elaborate.cxx:semantic_pass_symbols,
// but the needed declaration for module_cache is not available there.
// Nor for that matter in session.cxx.  Only in this CU is that field ever
// set (in query_module() above), so we clean it up here too.
static void
delete_session_module_cache (systemtap_session& s)
{
  if (s.module_cache) {
    if (s.verbose > 3)
      clog << _("deleting module_cache") << endl;
    delete s.module_cache;
    s.module_cache = 0;
  }
}


struct dwarf_var_expanding_visitor: public var_expanding_visitor
{
  dwarf_query & q;
  Dwarf_Die *scope_die;
  Dwarf_Addr addr;
  block *add_block;
  block *add_call_probe; // synthesized from .return probes with saved $vars
  // NB: tids are not always collected in add_block & add_call_probe, because
  // gen_kretprobe_saved_return doesn't need them.  Thus we need these extra
  // *_tid bools for gen_mapped_saved_return to tell what's there.
  bool add_block_tid, add_call_probe_tid;
  unsigned saved_longs, saved_strings; // data saved within kretprobes
  map<std::string, expression *> return_ts_map;
  vector<Dwarf_Die> scopes;
  // probe counter name -> pointer of associated probe
  std::set<std::string> perf_counter_refs;
  bool visited;

  dwarf_var_expanding_visitor(dwarf_query & q, Dwarf_Die *sd, Dwarf_Addr a):
    q(q), scope_die(sd), addr(a), add_block(NULL), add_call_probe(NULL),
    add_block_tid(false), add_call_probe_tid(false),
    saved_longs(0), saved_strings(0), visited(false) {}
  expression* gen_mapped_saved_return(expression* e, const string& name);
  expression* gen_kretprobe_saved_return(expression* e);
  void visit_target_symbol_saved_return (target_symbol* e);
  void visit_target_symbol_context (target_symbol* e);
  void visit_target_symbol (target_symbol* e);
  void visit_atvar_op (atvar_op* e);
  void visit_cast_op (cast_op* e);
  void visit_entry_op (entry_op* e);
  void visit_perf_op (perf_op* e);
private:
  vector<Dwarf_Die>& getscopes(target_symbol *e);
};


unsigned var_expanding_visitor::tick = 0;


var_expanding_visitor::var_expanding_visitor (): op()
{
  // FIXME: for the time being, by default we only support plain '$foo
  // = bar', not '+=' or any other op= variant. This is fixable, but a
  // bit ugly.
  //
  // If derived classes desire to add additional operator support, add
  // new operators to this list in the derived class constructor.
  valid_ops.insert ("=");
}


void
var_expanding_visitor::provide_lvalue_call(functioncall* fcall)
{
  // Provide the functioncall to our parent, so that it can be used to
  // substitute for the assignment node immediately above us.
  assert(!target_symbol_setter_functioncalls.empty());
  *(target_symbol_setter_functioncalls.top()) = fcall;
}


bool
var_expanding_visitor::rewrite_lvalue(const token* tok, const std::string& eop,
                                      expression*& lvalue, expression*& rvalue)
{
  // Our job would normally be to require() the left and right sides
  // into a new assignment. What we're doing is slightly trickier:
  // we're pushing a functioncall** onto a stack, and if our left
  // child sets the functioncall* for that value, we're going to
  // assume our left child was a target symbol -- transformed into a
  // set_target_foo(value) call, and it wants to take our right child
  // as the argument "value".
  //
  // This is why some people claim that languages with
  // constructor-decomposing case expressions have a leg up on
  // visitors.

  functioncall *fcall = NULL;

  // Let visit_target_symbol know what operator it should handle.
  const string* old_op = op;
  op = &eop;

  target_symbol_setter_functioncalls.push (&fcall);
  replace (lvalue);
  target_symbol_setter_functioncalls.pop ();
  replace (rvalue);

  op = old_op;

  if (fcall != NULL)
    {
      // Our left child is informing us that it was a target variable
      // and it has been replaced with a set_target_foo() function
      // call; we are going to provide that function call -- with the
      // right child spliced in as sole argument -- in place of
      // ourselves, in the var expansion we're in the middle of making.

      if (valid_ops.find (eop) == valid_ops.end ())
        {
	  // Build up a list of supported operators.
	  string ops;
	  std::set<string>::iterator i;
          int valid_ops_size = 0;
	  for (i = valid_ops.begin(); i != valid_ops.end(); i++)
          {
	    ops += " " + *i + ",";
            valid_ops_size++;
          }
	  ops.resize(ops.size() - 1);	// chop off the last ','

	  // Throw the error.
	  throw SEMANTIC_ERROR (_NF("Only the following assign operator is implemented on target variables: %s",
                                            "Only the following assign operators are implemented on target variables: %s",
                                           valid_ops_size, ops.c_str()), tok);

	}

      assert (lvalue == fcall);
      if (rvalue)
        fcall->args.push_back (rvalue);
      provide (fcall);
      return true;
    }
  else
    return false;
}


void
var_expanding_visitor::visit_assignment (assignment* e)
{
  if (!rewrite_lvalue (e->tok, e->op, e->left, e->right))
    provide (e);
}


void
var_expanding_visitor::visit_pre_crement (pre_crement* e)
{
  expression *dummy = NULL;
  if (!rewrite_lvalue (e->tok, e->op, e->operand, dummy))
    provide (e);
}


void
var_expanding_visitor::visit_post_crement (post_crement* e)
{
  expression *dummy = NULL;
  if (!rewrite_lvalue (e->tok, e->op, e->operand, dummy))
    provide (e);
}


void
var_expanding_visitor::visit_delete_statement (delete_statement* s)
{
  string fakeop = "delete";
  expression *dummy = NULL;
  if (!rewrite_lvalue (s->tok, fakeop, s->value, dummy))
    provide (s);
}


void
var_expanding_visitor::visit_defined_op (defined_op* e)
{
  bool resolved = true;

  defined_ops.push (e);
  try {
    replace (e->operand);

    // NB: Formerly, we had some curious cases to consider here, depending on what
    // various visit_target_symbol() implementations do for successful or
    // erroneous resolutions.  Some would signal a visit_target_symbol failure
    // with an exception, with a set flag within the target_symbol, or nothing
    // at all.
    //
    // Now, failures always have to be signalled with a
    // saved_conversion_error being chained to the target_symbol.
    // Successes have to result in an attempted rewrite of the
    // target_symbol (via provide()).
    //
    // Edna Mode: "no capes".  fche: "no exceptions".

    // dwarf stuff: success: rewrites to a function; failure: retains target_symbol, sets saved_conversion_error
    //
    // sdt-kprobes sdt.h: success: string or functioncall; failure: semantic_error
    //
    // sdt-uprobes: success: string or no op; failure: no op; expect derived/synthetic
    //              dwarf probe to take care of it.
    //              But this is rather unhelpful.  So we rig the sdt_var_expanding_visitor
    //              to pass through @defined() to the synthetic dwarf probe.
    //
    // utrace: success: rewrites to function; failure: semantic_error
    //
    // procfs: success: rewrites to function; failure: semantic_error

    target_symbol* tsym = dynamic_cast<target_symbol*> (e->operand);
    if (tsym && tsym->saved_conversion_error) // failing
      resolved = false;
    else if (tsym) // unresolved but not marked failing
      {
        // There are some visitors that won't touch certain target_symbols,
        // e.g. dwarf_var_expanding_visitor won't resolve @cast.  We should
        // leave it for now so some other visitor can have a chance.
        provide (e);
        return;
      }
    else // resolved, rewritten to some other expression type
      resolved = true;
  } catch (const semantic_error& e) {
    assert (0); // should not happen
  }
  defined_ops.pop ();

  literal_number* ln = new literal_number (resolved ? 1 : 0);
  ln->tok = e->tok;
  provide (ln);
}


struct dwarf_pretty_print
{
  dwarf_pretty_print (dwflpp& dw, vector<Dwarf_Die>& scopes, Dwarf_Addr pc,
                      const string& local, bool userspace_p,
                      const target_symbol& e):
    dw(dw), local(local), scopes(scopes), pc(pc), pointer(NULL),
    userspace_p(userspace_p), deref_p(true)
  {
    init_ts (e);
    dw.type_die_for_local (scopes, pc, local, ts, &base_type);
  }

  dwarf_pretty_print (dwflpp& dw, Dwarf_Die *scope_die, Dwarf_Addr pc,
                      bool userspace_p, const target_symbol& e):
    dw(dw), scopes(1, *scope_die), pc(pc), pointer(NULL),
    userspace_p(userspace_p), deref_p(true)
  {
    init_ts (e);
    dw.type_die_for_return (&scopes[0], pc, ts, &base_type);
  }

  dwarf_pretty_print (dwflpp& dw, Dwarf_Die *type_die, expression* pointer,
                      bool deref_p, bool userspace_p, const target_symbol& e):
    dw(dw), pc(0), pointer(pointer), pointer_type(*type_die),
    userspace_p(userspace_p), deref_p(deref_p)
  {
    init_ts (e);
    dw.type_die_for_pointer (type_die, ts, &base_type);
  }

  functioncall* expand ();
  ~dwarf_pretty_print () { delete ts; }

private:
  dwflpp& dw;
  target_symbol* ts;
  bool print_full;
  Dwarf_Die base_type;

  string local;
  vector<Dwarf_Die> scopes;
  Dwarf_Addr pc;

  expression* pointer;
  Dwarf_Die pointer_type;

  const bool userspace_p, deref_p;

  void recurse (Dwarf_Die* type, target_symbol* e,
                print_format* pf, bool top=false);
  void recurse_bitfield (Dwarf_Die* type, target_symbol* e,
                         print_format* pf);
  void recurse_base (Dwarf_Die* type, target_symbol* e,
                     print_format* pf);
  void recurse_array (Dwarf_Die* type, target_symbol* e,
                      print_format* pf, bool top);
  void recurse_pointer (Dwarf_Die* type, target_symbol* e,
                        print_format* pf, bool top);
  void recurse_struct (Dwarf_Die* type, target_symbol* e,
                       print_format* pf, bool top);
  void recurse_struct_members (Dwarf_Die* type, target_symbol* e,
                               print_format* pf, int& count);
  bool print_chars (Dwarf_Die* type, target_symbol* e, print_format* pf);

  void init_ts (const target_symbol& e);
  expression* deref (target_symbol* e);
  bool push_deref (print_format* pf, const string& fmt, target_symbol* e);
};


void
dwarf_pretty_print::init_ts (const target_symbol& e)
{
  // Work with a new target_symbol so we can modify arguments
  ts = new target_symbol (e);

  if (ts->addressof)
    throw SEMANTIC_ERROR(_("cannot take address of pretty-printed variable"), ts->tok);

  size_t depth = ts->pretty_print_depth ();
  if (depth == 0)
    throw SEMANTIC_ERROR(_("invalid target_symbol for pretty-print"), ts->tok);
  print_full = depth > 1;
  ts->components.pop_back();
}


functioncall*
dwarf_pretty_print::expand ()
{
  static unsigned tick = 0;

  // function pretty_print_X([pointer], [arg1, arg2, ...]) {
  //   try {
  //     return sprintf("{.foo=...}", (ts)->foo, ...)
  //   } catch {
  //     return "ERROR"
  //   }
  // }

  // Create the function decl and call.

  functiondecl *fdecl = new functiondecl;
  fdecl->tok = ts->tok;
  fdecl->synthetic = true;
  fdecl->name = "_dwarf_pretty_print_" + lex_cast(tick++);
  fdecl->type = pe_string;

  functioncall* fcall = new functioncall;
  fcall->referent = fdecl;
  fcall->tok = ts->tok;
  fcall->function = fdecl->name;
  fcall->type = pe_string;

  // If there's a <pointer>, replace it with a new var and make that
  // the first function argument.
  if (pointer)
    {
      vardecl *v = new vardecl;
      v->type = pe_long;
      v->name = "pointer";
      v->tok = ts->tok;
      fdecl->formal_args.push_back (v);
      fcall->args.push_back (pointer);

      symbol* sym = new symbol;
      sym->tok = ts->tok;
      sym->name = v->name;
      pointer = sym;
    }

  // For each expression argument, replace it with a function argument.
  for (unsigned i = 0; i < ts->components.size(); ++i)
    if (ts->components[i].type == target_symbol::comp_expression_array_index)
      {
        vardecl *v = new vardecl;
        v->type = pe_long;
        v->name = "index" + lex_cast(i);
        v->tok = ts->tok;
        fdecl->formal_args.push_back (v);
        fcall->args.push_back (ts->components[i].expr_index);

        symbol* sym = new symbol;
        sym->tok = ts->tok;
        sym->name = v->name;
        ts->components[i].expr_index = sym;
      }

  // Create the return sprintf.
  print_format* pf = print_format::create(ts->tok, "sprintf");
  return_statement* rs = new return_statement;
  rs->tok = ts->tok;
  rs->value = pf;

  // Recurse into the actual values.
  recurse (&base_type, ts, pf, true);
  pf->components = print_format::string_to_components(pf->raw_components);

  // Create the try-catch net
  try_block* tb = new try_block;
  tb->tok = ts->tok;
  tb->try_block = rs;
  tb->catch_error_var = 0;
  return_statement* rs2 = new return_statement;
  rs2->tok = ts->tok;
  rs2->value = new literal_string ("ERROR");
  rs2->value->tok = ts->tok;
  tb->catch_block = rs2;
  fdecl->body = tb;

  fdecl->join (dw.sess);
  return fcall;
}


void
dwarf_pretty_print::recurse (Dwarf_Die* start_type, target_symbol* e,
                             print_format* pf, bool top)
{
  // deal with initial void* pointers
  if (!deref_p && null_die(start_type))
    {
      push_deref (pf, "%p", e);
      return;
    }

  Dwarf_Die type;
  dw.resolve_unqualified_inner_typedie (start_type, &type, e);

  switch (dwarf_tag(&type))
    {
    default:
      // XXX need a warning?
      // throw semantic_error ("unsupported type (tag " + lex_cast(dwarf_tag(&type))
      //                       + ") for " + dwarf_type_name(&type), e->tok);
      pf->raw_components.append("?");
      break;

    case DW_TAG_enumeration_type:
    case DW_TAG_base_type:
      recurse_base (&type, e, pf);
      break;

    case DW_TAG_array_type:
      recurse_array (&type, e, pf, top);
      break;

    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      recurse_pointer (&type, e, pf, top);
      break;

    case DW_TAG_subroutine_type:
      push_deref (pf, "<function>:%p", e);
      break;

    case DW_TAG_union_type:
    case DW_TAG_structure_type:
    case DW_TAG_class_type:
      recurse_struct (&type, e, pf, top);
      break;
    }
}


// Bit fields are handled as a special-case combination of recurse() and
// recurse_base(), only called from recurse_struct_members().  The main
// difference is that the value is always printed numerically, even if the
// underlying type is a char.
void
dwarf_pretty_print::recurse_bitfield (Dwarf_Die* start_type, target_symbol* e,
                                      print_format* pf)
{
  Dwarf_Die type;
  dw.resolve_unqualified_inner_typedie (start_type, &type, e);

  int tag = dwarf_tag(&type);
  if (tag != DW_TAG_base_type && tag != DW_TAG_enumeration_type)
    {
      // XXX need a warning?
      // throw semantic_error ("unsupported bitfield type (tag " + lex_cast(tag)
      //                       + ") for " + dwarf_type_name(&type), e->tok);
      pf->raw_components.append("?");
      return;
    }

  Dwarf_Attribute attr;
  Dwarf_Word encoding = (Dwarf_Word) -1;
  dwarf_formudata (dwarf_attr_integrate (&type, DW_AT_encoding, &attr),
                   &encoding);
  switch (encoding)
    {
    case DW_ATE_float:
    case DW_ATE_complex_float:
      // XXX need a warning?
      // throw semantic_error ("unsupported bitfield type (encoding " + lex_cast(encoding)
      //                       + ") for " + dwarf_type_name(&type), e->tok);
      pf->raw_components.append("?");
      break;

    case DW_ATE_unsigned:
    case DW_ATE_unsigned_char:
      push_deref (pf, "%u", e);
      break;

    case DW_ATE_signed:
    case DW_ATE_signed_char:
    default:
      push_deref (pf, "%i", e);
      break;
    }
}


void
dwarf_pretty_print::recurse_base (Dwarf_Die* type, target_symbol* e,
                                  print_format* pf)
{
  Dwarf_Attribute attr;
  Dwarf_Word encoding = (Dwarf_Word) -1;
  dwarf_formudata (dwarf_attr_integrate (type, DW_AT_encoding, &attr),
                   &encoding);
  switch (encoding)
    {
    case DW_ATE_float:
    case DW_ATE_complex_float:
      // XXX need a warning?
      // throw semantic_error ("unsupported type (encoding " + lex_cast(encoding)
      //                       + ") for " + dwarf_type_name(type), e->tok);
      pf->raw_components.append("?");
      break;

    case DW_ATE_UTF: // XXX need to add unicode to _stp_vsprint_char
    case DW_ATE_signed_char:
    case DW_ATE_unsigned_char:
      // Use escapes to make sure that non-printable characters
      // don't interrupt our stream (especially '\0' values).
      push_deref (pf, "'%#c'", e);
      break;

    case DW_ATE_unsigned:
      push_deref (pf, "%u", e);
      break;

    case DW_ATE_signed:
    default:
      push_deref (pf, "%i", e);
      break;
    }
}


void
dwarf_pretty_print::recurse_array (Dwarf_Die* type, target_symbol* e,
                                   print_format* pf, bool top)
{
  if (!top && !print_full)
    {
      pf->raw_components.append("[...]");
      return;
    }

  Dwarf_Die childtype;
  dwarf_attr_die (type, DW_AT_type, &childtype);

  if (print_chars (&childtype, e, pf))
    return;

  pf->raw_components.append("[");

  // We print the array up to the first 5 elements.
  // XXX how can we determine the array size?
  // ... for now, just print the first element
  // NB: limit to 32 args; see PR10750 and c_unparser::visit_print_format.
  unsigned i, size = 1;
  for (i=0; i < size && i < 5 && pf->args.size() < 32; ++i)
    {
      if (i > 0)
        pf->raw_components.append(", ");
      target_symbol* e2 = new target_symbol(*e);
      e2->components.push_back (target_symbol::component(e->tok, i));
      recurse (&childtype, e2, pf);
    }
  if (i < size || 1/*XXX until real size is known */)
    pf->raw_components.append(", ...");
  pf->raw_components.append("]");
}


void
dwarf_pretty_print::recurse_pointer (Dwarf_Die* type, target_symbol* e,
                                     print_format* pf, bool top)
{
  // We chase to top-level pointers, but leave the rest alone
  bool void_p = true;
  Dwarf_Die pointee;
  if (dwarf_attr_die (type, DW_AT_type, &pointee))
    {
      try
        {
          dw.resolve_unqualified_inner_typedie (&pointee, &pointee, e);
          void_p = false;
        }
      catch (const semantic_error&) {}
    }

  if (!void_p)
    {
      if (print_chars (&pointee, e, pf))
        return;

      if (top)
        {
          recurse (&pointee, e, pf, top);
          return;
        }
    }

  push_deref (pf, "%p", e);
}


void
dwarf_pretty_print::recurse_struct (Dwarf_Die* type, target_symbol* e,
                                    print_format* pf, bool top)
{
  if (dwarf_hasattr(type, DW_AT_declaration))
    {
      Dwarf_Die *resolved = dw.declaration_resolve(type);
      if (!resolved)
        {
          // could be an error, but for now just stub it
          // throw semantic_error ("unresolved " + dwarf_type_name(type), e->tok);
          pf->raw_components.append("{...}");
          return;
        }
      type = resolved;
    }

  int count = 0;
  pf->raw_components.append("{");
  if (top || print_full)
    recurse_struct_members (type, e, pf, count);
  else
    pf->raw_components.append("...");
  pf->raw_components.append("}");
}


void
dwarf_pretty_print::recurse_struct_members (Dwarf_Die* type, target_symbol* e,
                                            print_format* pf, int& count)
{
  /* With inheritance, a subclass may mask member names of parent classes, so
   * our search among the inheritance tree must be breadth-first rather than
   * depth-first (recursive).  The type die is still our starting point.  When
   * we encounter a masked name, just skip it. */
  set<string> dupes;
  deque<Dwarf_Die> inheritees(1, *type);
  for (; !inheritees.empty(); inheritees.pop_front())
    {
      Dwarf_Die child, childtype, import;
      if (dwarf_child (&inheritees.front(), &child) == 0)
        do
          {
            target_symbol* e2 = e;

            // skip static members
            if (dwarf_hasattr(&child, DW_AT_declaration))
              continue;

            int tag = dwarf_tag (&child);

            /* Pretend imported units contain members by recursing into
               struct_member printing with the same count. */
            if (tag == DW_TAG_imported_unit
                && dwarf_attr_die (&child, DW_AT_import, &import))
              recurse_struct_members (&import, e2, pf, count);

            if (tag != DW_TAG_member && tag != DW_TAG_inheritance)
              continue;

            dwarf_attr_die (&child, DW_AT_type, &childtype);

            if (tag == DW_TAG_inheritance)
              {
                inheritees.push_back(childtype);
                continue;
              }

            int childtag = dwarf_tag (&childtype);
            const char *member = dwarf_diename (&child);

            // "_vptr.foo" members are C++ virtual function tables,
            // which (generally?) aren't interesting for users.
            if (member && startswith(member, "_vptr."))
              continue;

            // skip inheritance-masked duplicates
            if (member && !dupes.insert(member).second)
              continue;

            if (++count > 1)
              pf->raw_components.append(", ");

            // NB: limit to 32 args; see PR10750 and c_unparser::visit_print_format.
            if (pf->args.size() >= 32)
              {
                pf->raw_components.append("...");
                break;
              }

            if (member)
              {
                pf->raw_components.append(".");
                pf->raw_components.append(member);

                e2 = new target_symbol(*e);
                e2->components.push_back (target_symbol::component(e->tok, member));
              }
            else if (childtag == DW_TAG_union_type)
              pf->raw_components.append("<union>");
            else if (childtag == DW_TAG_structure_type)
              pf->raw_components.append("<class>");
            else if (childtag == DW_TAG_class_type)
              pf->raw_components.append("<struct>");
            pf->raw_components.append("=");

            if (dwarf_hasattr_integrate (&child, DW_AT_bit_offset))
              recurse_bitfield (&childtype, e2, pf);
            else
              recurse (&childtype, e2, pf);
          }
        while (dwarf_siblingof (&child, &child) == 0);
    }
}


bool
dwarf_pretty_print::print_chars (Dwarf_Die* start_type, target_symbol* e,
                                 print_format* pf)
{
  Dwarf_Die type;
  dw.resolve_unqualified_inner_typedie (start_type, &type, e);

  Dwarf_Attribute attr;
  Dwarf_Word encoding = (Dwarf_Word) -1;
  dwarf_formudata (dwarf_attr_integrate (&type, DW_AT_encoding, &attr),
                   &encoding);
  switch (encoding)
    {
    case DW_ATE_UTF:
    case DW_ATE_signed_char:
    case DW_ATE_unsigned_char:
      break;
    default:
      return false;
    }

  string function = userspace_p ? "user_string2" : "kernel_string2";
  Dwarf_Word size = (Dwarf_Word) -1;
  dwarf_formudata (dwarf_attr_integrate (&type, DW_AT_byte_size, &attr), &size);
  switch (size)
    {
    case 1:
      break;
    case 2:
      function += "_utf16";
      break;
    case 4:
      function += "_utf32";
      break;
    default:
      return false;
    }

  if (push_deref (pf, "\"%s\"", e))
    {
      // steal the last arg for a string access
      assert (!pf->args.empty());
      functioncall* fcall = new functioncall;
      fcall->tok = e->tok;
      fcall->function = function;
      fcall->args.push_back (pf->args.back());
      expression *err_msg = new literal_string ("<unknown>");
      err_msg->tok = e->tok;
      fcall->args.push_back (err_msg);
      pf->args.back() = fcall;
    }
  return true;
}

// PR10601: adapt to kernel-vs-userspace loc2c-runtime
static const string EMBEDDED_FETCH_DEREF_KERNEL = string("\n")
  + "#define fetch_register k_fetch_register\n"
  + "#define store_register k_store_register\n"
  + "#define deref kderef\n"
  + "#define store_deref store_kderef\n";

static const string EMBEDDED_FETCH_DEREF_USER = string("\n")
  + "#define fetch_register u_fetch_register\n"
  + "#define store_register u_store_register\n"
  + "#define deref uderef\n"
  + "#define store_deref store_uderef\n";

#define EMBEDDED_FETCH_DEREF(U) \
  (U ? EMBEDDED_FETCH_DEREF_USER : EMBEDDED_FETCH_DEREF_KERNEL)

static const string EMBEDDED_FETCH_DEREF_DONE = string("\n")
  + "#undef fetch_register\n"
  + "#undef store_register\n"
  + "#undef deref\n"
  + "#undef store_deref\n";

static functioncall*
synthetic_embedded_deref_call(dwflpp& dw,
                              Dwarf_Die* function_type,
                              const string& function_name,
                              const string& function_code,
                              bool userspace_p,
                              bool lvalue_p,
                              target_symbol* e,
                              expression* pointer=NULL)
{
  // Synthesize a functiondecl for the given embedded code string.
  functiondecl *fdecl = new functiondecl;
  fdecl->synthetic = true;
  fdecl->tok = e->tok;
  fdecl->name = function_name;
  // The fdecl type is generic, but we'll be detailed on the fcall below.
  fdecl->type = pe_long;
  fdecl->type_details.reset(new exp_type_dwarf(&dw, function_type,
                                               userspace_p, e->addressof));

  embeddedcode *ec = new embeddedcode;
  ec->tok = e->tok;
  ec->code += "/* unprivileged */";
  if (! lvalue_p)
    ec->code += "/* pure */";
  ec->code += EMBEDDED_FETCH_DEREF(userspace_p);
  ec->code += function_code;
  ec->code += EMBEDDED_FETCH_DEREF_DONE;
  fdecl->body = ec;

  // Synthesize a functioncall.
  functioncall* fcall = new functioncall;
  fcall->tok = e->tok;
  fcall->referent = fdecl;
  fcall->function = fdecl->name;
  fcall->type = fdecl->type;
  fcall->type_details = fdecl->type_details;

  // If this code snippet uses a precomputed pointer,
  // pass that as the first argument.
  if (pointer)
    {
      vardecl *v = new vardecl;
      v->type = pe_long;
      v->name = "pointer";
      v->tok = e->tok;
      fdecl->formal_args.push_back(v);
      fcall->args.push_back(pointer);
    }

  // Any non-literal indexes need to be passed as arguments too.
  for (unsigned i = 0; i < e->components.size(); ++i)
    if (e->components[i].type == target_symbol::comp_expression_array_index)
      {
        vardecl *v = new vardecl;
        v->type = pe_long;
        v->name = "index" + lex_cast(i);
        v->tok = e->tok;
        fdecl->formal_args.push_back(v);
        fcall->args.push_back(e->components[i].expr_index);
      }

  // If this code snippet is assigning to an lvalue,
  // add a final argument for the rvalue.
  if (lvalue_p)
    {
      // Modify the fdecl so it carries a single pe_long formal
      // argument called "value".

      // FIXME: For the time being we only support setting target
      // variables which have base types; these are 'pe_long' in
      // stap's type vocabulary.  Strings and pointers might be
      // reasonable, some day, but not today.

      vardecl *v = new vardecl;
      v->type = pe_long;
      v->name = "value";
      v->tok = e->tok;
      fdecl->formal_args.push_back(v);
      // NB: We don't know the value for fcall argument yet.
      // (see target_symbol_setter_functioncalls)
    }

  // Add the synthesized decl to the session, and return the call.
  fdecl->join (dw.sess);
  return fcall;
}

expression*
dwarf_pretty_print::deref (target_symbol* e)
{
  static unsigned tick = 0;

  if (!deref_p)
    {
      assert (pointer && e->components.empty());
      return pointer;
    }

  bool lvalue_p = false;
  string name = "_dwarf_pretty_print_deref_" + lex_cast(tick++);

  string code;
  Dwarf_Die endtype;
  if (pointer)
    code = dw.literal_stmt_for_pointer (&pointer_type, e, false, &endtype);
  else if (!local.empty())
    code = dw.literal_stmt_for_local (scopes, pc, local, e, false, &endtype);
  else
    code = dw.literal_stmt_for_return (&scopes[0], pc, e, false, &endtype);

  return synthetic_embedded_deref_call(dw, &endtype, name, code,
                                       userspace_p, lvalue_p, e, pointer);
}


bool
dwarf_pretty_print::push_deref (print_format* pf, const string& fmt,
                                target_symbol* e)
{
  expression* e2 = NULL;
  try
    {
      e2 = deref (e);
    }
  catch (const semantic_error&)
    {
      pf->raw_components.append ("?");
      return false;
    }
  pf->raw_components.append (fmt);
  pf->args.push_back (e2);
  return true;
}


void
dwarf_var_expanding_visitor::visit_target_symbol_saved_return (target_symbol* e)
{
  // Get the full name of the target symbol.
  stringstream ts_name_stream;
  e->print(ts_name_stream);
  string ts_name = ts_name_stream.str();

  // Check and make sure we haven't already seen this target
  // variable in this return probe.  If we have, just return our
  // last replacement.
  map<string, expression *>::iterator i = return_ts_map.find(ts_name);
  if (i != return_ts_map.end())
    {
      provide (i->second);
      return;
    }

  // Attempt the expansion directly first, so if there's a problem with the
  // variable we won't have a bogus entry probe lying around.  Like in
  // saveargs(), we pretend for a moment that we're not in a .return.
  bool saved_has_return = q.has_return;
  q.has_return = false;
  expression *repl = e;
  replace (repl);
  q.has_return = saved_has_return;

  // If it's still a target_symbol, then it couldn't be resolved.  It may
  // not have a saved_conversion_error yet, e.g. for null_die(scope_die),
  // but we know it's not worth making that bogus entry anyway.
  if (dynamic_cast<target_symbol*>(repl))
    {
      provide (repl);
      return;
    }

  expression *exp;
  if (!q.has_process &&
      strverscmp(q.sess.kernel_base_release.c_str(), "2.6.25") >= 0)
    exp = gen_kretprobe_saved_return(repl);
  else
    exp = gen_mapped_saved_return(repl, e->sym_name());

  // Propagate the DWARF type to the expression in the return probe.
  if (repl->type_details && !exp->type_details)
    exp->type_details = repl->type_details;

  // Provide the variable to our parent so it can be used as a
  // substitute for the target symbol.
  provide (exp);

  // Remember this replacement since we might be able to reuse
  // it later if the same return probe references this target
  // symbol again.
  return_ts_map[ts_name] = exp;
}

static expression*
gen_mapped_saved_return(systemtap_session &sess, expression* e,
			const string& name,
			block *& add_block, bool& add_block_tid,
			block *& add_call_probe, bool& add_call_probe_tid)
{
  static unsigned tick = 0;

  // We've got to do several things here to handle target
  // variables in return probes.

  // (1) Synthesize two global arrays.  One is the cache of the
  // target variable and the other contains a thread specific
  // nesting level counter.  The arrays will look like
  // this:
  //
  //   _entry_tvar_{name}_{num}
  //   _entry_tvar_{name}_{num}_ctr

  string aname = (string("_entry_tvar_")
                  + name
                  + "_" + lex_cast(tick++));
  vardecl* vd = new vardecl;
  vd->name = aname;
  vd->tok = e->tok;
  sess.globals.push_back (vd);

  string ctrname = aname + "_ctr";
  vd = new vardecl;
  vd->name = ctrname;
  vd->tok = e->tok;
  sess.globals.push_back (vd);

  // (2) Create a new code block we're going to insert at the
  // beginning of this probe to get the cached value into a
  // temporary variable.  We'll replace the target variable
  // reference with the temporary variable reference.  The code
  // will look like this:
  //
  //   _entry_tvar_tid = tid()
  //   _entry_tvar_{name}_{num}_tmp
  //       = _entry_tvar_{name}_{num}[_entry_tvar_tid,
  //                    _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]]
  //   delete _entry_tvar_{name}_{num}[_entry_tvar_tid,
  //                    _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]--]
  //   if (! _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid])
  //       delete _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]

  // (2a) Synthesize the tid temporary expression, which will look
  // like this:
  //
  //   _entry_tvar_tid = tid()
  symbol* tidsym = new symbol;
  tidsym->name = string("_entry_tvar_tid");
  tidsym->tok = e->tok;

  if (add_block == NULL)
    {
      add_block = new block;
      add_block->tok = e->tok;
    }

  if (!add_block_tid)
    {
      // Synthesize a functioncall to grab the thread id.
      functioncall* fc = new functioncall;
      fc->tok = e->tok;
      fc->function = string("tid");

      // Assign the tid to '_entry_tvar_tid'.
      assignment* a = new assignment;
      a->tok = e->tok;
      a->op = "=";
      a->left = tidsym;
      a->right = fc;

      expr_statement* es = new expr_statement;
      es->tok = e->tok;
      es->value = a;
      add_block->statements.push_back (es);
      add_block_tid = true;
    }

  // (2b) Synthesize an array reference and assign it to a
  // temporary variable (that we'll use as replacement for the
  // target variable reference).  It will look like this:
  //
  //   _entry_tvar_{name}_{num}_tmp
  //       = _entry_tvar_{name}_{num}[_entry_tvar_tid,
  //                    _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]]

  arrayindex* ai_tvar_base = new arrayindex;
  ai_tvar_base->tok = e->tok;

  symbol* sym = new symbol;
  sym->name = aname;
  sym->tok = e->tok;
  ai_tvar_base->base = sym;

  ai_tvar_base->indexes.push_back(tidsym);

  // We need to create a copy of the array index in its current
  // state so we can have 2 variants of it (the original and one
  // that post-decrements the second index).
  arrayindex* ai_tvar = new arrayindex;
  arrayindex* ai_tvar_postdec = new arrayindex;
  *ai_tvar = *ai_tvar_base;
  *ai_tvar_postdec = *ai_tvar_base;

  // Synthesize the
  // "_entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]" used as the
  // second index into the array.
  arrayindex* ai_ctr = new arrayindex;
  ai_ctr->tok = e->tok;

  sym = new symbol;
  sym->name = ctrname;
  sym->tok = e->tok;
  ai_ctr->base = sym;
  ai_ctr->indexes.push_back(tidsym);
  ai_tvar->indexes.push_back(ai_ctr);

  symbol* tmpsym = new symbol;
  tmpsym->name = aname + "_tmp";
  tmpsym->tok = e->tok;

  assignment* a = new assignment;
  a->tok = e->tok;
  a->op = "=";
  a->left = tmpsym;
  a->right = ai_tvar;

  expr_statement* es = new expr_statement;
  es->tok = e->tok;
  es->value = a;

  add_block->statements.push_back (es);

  // (2c) Add a post-decrement to the second array index and
  // delete the array value.  It will look like this:
  //
  //   delete _entry_tvar_{name}_{num}[_entry_tvar_tid,
  //                    _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]--]

  post_crement* pc = new post_crement;
  pc->tok = e->tok;
  pc->op = "--";
  pc->operand = ai_ctr;
  ai_tvar_postdec->indexes.push_back(pc);

  delete_statement* ds = new delete_statement;
  ds->tok = e->tok;
  ds->value = ai_tvar_postdec;

  add_block->statements.push_back (ds);

  // (2d) Delete the counter value if it is 0.  It will look like
  // this:
  //   if (! _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid])
  //       delete _entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]

  ds = new delete_statement;
  ds->tok = e->tok;
  ds->value = ai_ctr;

  unary_expression *ue = new unary_expression;
  ue->tok = e->tok;
  ue->op = "!";
  ue->operand = ai_ctr;

  if_statement *ifs = new if_statement;
  ifs->tok = e->tok;
  ifs->condition = ue;
  ifs->thenblock = ds;
  ifs->elseblock = NULL;

  add_block->statements.push_back (ifs);

  // (3) We need an entry probe that saves the value for us in the
  // global array we created.  Create the entry probe, which will
  // look like this:
  //
  //   probe kernel.function("{function}").call {
  //     _entry_tvar_tid = tid()
  //     _entry_tvar_{name}_{num}[_entry_tvar_tid,
  //                       ++_entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]]
  //       = ${param}
  //   }

  if (add_call_probe == NULL)
    {
      add_call_probe = new block;
      add_call_probe->tok = e->tok;
    }

  if (!add_call_probe_tid)
    {
      // Synthesize a functioncall to grab the thread id.
      functioncall* fc = new functioncall;
      fc->tok = e->tok;
      fc->function = string("tid");

      // Assign the tid to '_entry_tvar_tid'.
      assignment* a = new assignment;
      a->tok = e->tok;
      a->op = "=";
      a->left = tidsym;
      a->right = fc;

      expr_statement* es = new expr_statement;
      es->tok = e->tok;
      es->value = a;
      add_call_probe = new block(add_call_probe, es);
      add_call_probe_tid = true;
    }

  // Save the value, like this:
  //     _entry_tvar_{name}_{num}[_entry_tvar_tid,
  //                       ++_entry_tvar_{name}_{num}_ctr[_entry_tvar_tid]]
  //       = ${param}
  arrayindex* ai_tvar_preinc = new arrayindex;
  *ai_tvar_preinc = *ai_tvar_base;

  pre_crement* preinc = new pre_crement;
  preinc->tok = e->tok;
  preinc->op = "++";
  preinc->operand = ai_ctr;
  ai_tvar_preinc->indexes.push_back(preinc);

  a = new assignment;
  a->tok = e->tok;
  a->op = "=";
  a->left = ai_tvar_preinc;
  a->right = e;

  es = new expr_statement;
  es->tok = e->tok;
  es->value = a;

  add_call_probe = new block(add_call_probe, es);

  // (4) Provide the '_entry_tvar_{name}_{num}_tmp' variable to
  // our parent so it can be used as a substitute for the target
  // symbol.
  delete ai_tvar_base;
  return tmpsym;
}


expression*
dwarf_var_expanding_visitor::gen_mapped_saved_return(expression* e,
                                                     const string& name)
{
    return ::gen_mapped_saved_return(q.sess, e, name, add_block,
				     add_block_tid, add_call_probe,
				     add_call_probe_tid);
}


expression*
dwarf_var_expanding_visitor::gen_kretprobe_saved_return(expression* e)
{
  // The code for this is simple.
  //
  // .call:
  //   _set_kretprobe_long(index, $value)
  //
  // .return:
  //   _get_kretprobe_long(index)
  //
  // (or s/long/string/ for things like $$parms)

  unsigned index;
  string setfn, getfn;

  // We need the caller to predetermine the type of the expression!
  switch (e->type)
    {
    case pe_string:
      index = saved_strings++;
      setfn = "_set_kretprobe_string";
      getfn = "_get_kretprobe_string";
      break;
    case pe_long:
      index = saved_longs++;
      setfn = "_set_kretprobe_long";
      getfn = "_get_kretprobe_long";
      break;
    default:
      throw SEMANTIC_ERROR(_("unknown type to save in kretprobe"), e->tok);
    }

  // Create the entry code
  //   _set_kretprobe_{long|string}(index, $value)

  if (add_call_probe == NULL)
    {
      add_call_probe = new block;
      add_call_probe->tok = e->tok;
    }

  functioncall* set_fc = new functioncall;
  set_fc->tok = e->tok;
  set_fc->function = setfn;
  set_fc->args.push_back(new literal_number(index));
  set_fc->args.back()->tok = e->tok;
  set_fc->args.push_back(e);

  expr_statement* set_es = new expr_statement;
  set_es->tok = e->tok;
  set_es->value = set_fc;

  add_call_probe->statements.push_back(set_es);

  // Create the return code
  //   _get_kretprobe_{long|string}(index)

  functioncall* get_fc = new functioncall;
  get_fc->tok = e->tok;
  get_fc->function = getfn;
  get_fc->args.push_back(new literal_number(index));
  get_fc->args.back()->tok = e->tok;

  return get_fc;
}


void
dwarf_var_expanding_visitor::visit_target_symbol_context (target_symbol* e)
{
  if (pending_interrupts) {
    provide(e);
    return;
  }

  if (null_die(scope_die)) {
    literal_string *empty = new literal_string("");
    empty->tok = e->tok;
    provide(empty);
    return;
  }

  target_symbol *tsym = new target_symbol(*e);

  bool pretty = e->check_pretty_print ();
  string format = pretty ? "=%s" : "=%#x";

  // Convert $$parms to sprintf of a list of parms and active local vars
  // which we recursively evaluate

  print_format* pf = print_format::create(e->tok, "sprintf");

  if (q.has_return && (e->name == "$$return"))
    {
      tsym->name = "$return";

      // Ignore any variable that isn't accessible.
      tsym->saved_conversion_error = 0;
      expression *texp = tsym;
      replace (texp); // NB: throws nothing ...
      if (tsym->saved_conversion_error) // ... but this is how we know it happened.
        {

        }
      else
        {
          pf->raw_components += "return";
          pf->raw_components += format;
          pf->args.push_back(texp);
        }
    }
  else
    {
      // non-.return probe: support $$parms, $$vars, $$locals
      bool first = true;
      Dwarf_Die result;
      vector<Dwarf_Die> scopes = q.dw.getscopes(scope_die);
      for (unsigned i = 0; i < scopes.size(); ++i)
        {
          if (dwarf_tag(&scopes[i]) == DW_TAG_compile_unit)
            break; // we don't want file-level variables
          if (dwarf_child (&scopes[i], &result) == 0)
            do
              {
                switch (dwarf_tag (&result))
                  {
                  case DW_TAG_variable:
                    if (e->name == "$$parms")
                      continue;
                    break;
                  case DW_TAG_formal_parameter:
                    if (e->name == "$$locals")
                      continue;
                    break;

                  default:
                    continue;
                  }

                const char *diename = dwarf_diename (&result);
                if (! diename) continue;

                if (! first)
                  pf->raw_components += " ";
                pf->raw_components += diename;
                first = false;

                // Write a placeholder for ugly aggregates
                Dwarf_Die type;
                if (!pretty && dwarf_attr_die(&result, DW_AT_type, &type))
                  {
                    q.dw.resolve_unqualified_inner_typedie(&type, &type, e);
                    switch (dwarf_tag(&type))
                      {
                      case DW_TAG_union_type:
                      case DW_TAG_structure_type:
                      case DW_TAG_class_type:
                        pf->raw_components += "={...}";
                        continue;

                      case DW_TAG_array_type:
                        pf->raw_components += "=[...]";
                        continue;
                      }
                  }

                tsym->name = "$";
                tsym->name += diename;

                // Ignore any variable that isn't accessible.
                tsym->saved_conversion_error = 0;
                expression *texp = tsym;
                replace (texp); // NB: throws nothing ...
                if (tsym->saved_conversion_error) // ... but this is how we know it happened.
                  {
                    if (q.sess.verbose>2)
                      {
                        for (const semantic_error *c = tsym->saved_conversion_error;
                             c != 0;
                             c = c->get_chain()) {
                            clog << _("variable location problem [man error::dwarf]: ") << c->what() << endl;
                        }
                      }

                    pf->raw_components += "=?";
                  }
                else
                  {
                    pf->raw_components += format;
                    pf->args.push_back(texp);
                  }
              }
            while (dwarf_siblingof (&result, &result) == 0);
        }
    }

  pf->components = print_format::string_to_components(pf->raw_components);
  pf->type = pe_string;
  provide (pf);
}


void
dwarf_var_expanding_visitor::visit_atvar_op (atvar_op *e)
{
  // Fill in our current module context if needed
  if (e->module.empty())
    e->module = q.dw.module_name;

  if (e->module == q.dw.module_name && e->cu_name.empty())
    {
      // process like any other local
      // e->sym_name() will do the right thing
      visit_target_symbol(e);
      return;
    }

  var_expanding_visitor::visit_atvar_op(e);
}


void
dwarf_var_expanding_visitor::visit_target_symbol (target_symbol *e)
{
  assert(e->name.size() > 0 && (e->name[0] == '$' || e->name == "@var"));
  visited = true;
  bool defined_being_checked = (defined_ops.size() > 0 && (defined_ops.top()->operand == e));
  // In this mode, we avoid hiding errors or generating extra code such as for .return saved $vars

  try
    {
      bool lvalue = is_active_lvalue(e);
      if (lvalue && !q.sess.guru_mode)
        throw SEMANTIC_ERROR(_("write to target variable not permitted; need stap -g"), e->tok);

      // XXX: process $context vars should be writable

      // See if we need to generate a new probe to save/access function
      // parameters from a return probe.  PR 1382.
      if (q.has_return
          && !defined_being_checked
          && e->name != "$return" // not the special return-value variable handled below
          && e->name != "$$return") // nor the other special variable handled below
        {
          if (lvalue)
            throw SEMANTIC_ERROR(_("write to target variable not permitted in .return probes"), e->tok);
          visit_target_symbol_saved_return(e);
          return;
        }

      if (e->name == "$$vars" || e->name == "$$parms" || e->name == "$$locals"
          || (q.has_return && (e->name == "$$return")))
        {
          if (lvalue)
            throw SEMANTIC_ERROR(_("cannot write to context variable"), e->tok);

          if (e->addressof)
            throw SEMANTIC_ERROR(_("cannot take address of context variable"), e->tok);

          e->assert_no_components("dwarf", true);

          visit_target_symbol_context(e);
          return;
        }

      // Everything else (pretty-printed vars, and context vars) require a
      // scope_die in which to search for them. If we don't have that, just
      // leave it unresolved; we'll produce an error later on.
      if (null_die(scope_die))
        {
          provide(e);
          return;
        }

      if (e->check_pretty_print (lvalue))
        {
          if (q.has_return && (e->name == "$return"))
            {
              dwarf_pretty_print dpp (q.dw, scope_die, addr,
                                      q.has_process, *e);
              dpp.expand()->visit(this);
            }
          else
            {
              dwarf_pretty_print dpp (q.dw, getscopes(e), addr,
                                      e->sym_name(),
                                      q.has_process, *e);
              dpp.expand()->visit(this);
            }
          return;
        }

      bool userspace_p = q.has_process;
      string fname = (string(lvalue ? "_dwarf_tvar_set" : "_dwarf_tvar_get")
                      + "_" + escaped_indentifier_string (e->sym_name())
                      + "_" + lex_cast(tick++));


      string code;
      Dwarf_Die endtype;
      if (q.has_return && (e->name == "$return"))
        code = q.dw.literal_stmt_for_return (scope_die, addr, e, lvalue, &endtype);
      else
        code = q.dw.literal_stmt_for_local (getscopes(e), addr, e->sym_name(),
                                            e, lvalue, &endtype);

      functioncall* n = synthetic_embedded_deref_call(q.dw, &endtype, fname, code,
                                                      userspace_p, lvalue, e);

      if (lvalue)
	provide_lvalue_call (n);

      // Revisit the functioncall so arguments can be expanded.
      n->visit (this);
    }
  catch (const semantic_error& er)
    {
      // We suppress this error message, and pass the unresolved
      // target_symbol to the next pass.  We hope that this value ends
      // up not being referenced after all, so it can be optimized out
      // quietly.
      e->chain (er);
      provide (e);
    }
}


void
dwarf_var_expanding_visitor::visit_cast_op (cast_op *e)
{
  // Fill in our current module context if needed
  if (e->module.empty())
    e->module = q.dw.module_name;

  var_expanding_visitor::visit_cast_op(e);
}


void
dwarf_var_expanding_visitor::visit_entry_op (entry_op *e)
{
  expression *repl = e;
  if (q.has_return)
    {
      // expand the operand as if it weren't a return probe
      q.has_return = false;
      replace (e->operand);
      q.has_return = true;

      // XXX it would be nice to use gen_kretprobe_saved_return when available,
      // but it requires knowing the types already, which is problematic for
      // arbitrary expressons.
      repl = gen_mapped_saved_return (e->operand, "entry");
    }
  provide (repl);
}

void
dwarf_var_expanding_visitor::visit_perf_op (perf_op *e)
{
  string e_lit_val = e->operand->value;

  add_block = new block;
  add_block->tok = e->tok;

  systemtap_session &s = this->q.sess;
  std::vector<std::pair<std::string,std::string> >::iterator it;
  // Find the associated perf.counter probe
  for (it=s.perf_counters.begin();
       it != s.perf_counters.end();
       it++)
    if ((*it).first == e_lit_val)
      {
	// if perf .process("name") omitted, then set it to this process name
	if ((*it).second.length() == 0)
	  (*it).second = this->q.user_path;
	if ((*it).second == this->q.user_path)
	  break;
      }

  if (it != s.perf_counters.end())
    {
      perf_counter_refs.insert((*it).first);
      // __perf_read_N is assigned in the probe prologue
      symbol* sym = new symbol;
      sym->tok = e->tok;
      sym->name = "__perf_read_" + (*it).first;
      provide (sym);
    }
  else
    throw SEMANTIC_ERROR(_F("perf counter '%s' not defined", e_lit_val.c_str()));
}


vector<Dwarf_Die>&
dwarf_var_expanding_visitor::getscopes(target_symbol *e)
{
  if (scopes.empty())
    {
      if(!null_die(scope_die))
        scopes = q.dw.getscopes(scope_die);
      if (scopes.empty())
        //throw semantic_error (_F("unable to find any scopes containing %d", addr), e->tok);
        //                        ((scope_die == NULL) ? "" : (string (" in ") + (dwarf_diename(scope_die) ?: "<unknown>") + "(" + (dwarf_diename(q.dw.cu) ?: "<unknown>") ")" ))
        throw SEMANTIC_ERROR ("unable to find any scopes containing "
                              + lex_cast_hex(addr)
                              + (null_die(scope_die) ? ""
                                 : (string (" in ")
                                    + (dwarf_diename(scope_die) ?: "<unknown>")
                                    + "(" + (dwarf_diename(q.dw.cu) ?: "<unknown>")
                                    + ")"))
                              + " while searching for local '"
                              + e->sym_name() + "'",
                              e->tok);
    }
  return scopes;
}


struct dwarf_cast_expanding_visitor: public var_expanding_visitor
{
  systemtap_session& s;
  dwarf_builder& db;
  map<string,string> compiled_headers;

  dwarf_cast_expanding_visitor(systemtap_session& s, dwarf_builder& db):
    s(s), db(db) {}
  void visit_cast_op (cast_op* e);
  void filter_special_modules(string& module);
};


struct dwarf_cast_query : public base_query
{
  cast_op& e;
  const bool lvalue;
  const bool userspace_p;
  functioncall*& result;

  dwarf_cast_query(dwflpp& dw, const string& module, cast_op& e, bool lvalue,
                   const bool userspace_p, functioncall*& result):
    base_query(dw, module), e(e), lvalue(lvalue),
    userspace_p(userspace_p), result(result) {}

  void handle_query_module();
  void query_library (const char *) {}
  void query_plt (const char *entry, size_t addr) {}
};


void
dwarf_cast_query::handle_query_module()
{
  static unsigned tick = 0;

  if (result)
    return;

  // look for the type in any CU
  Dwarf_Die* type_die = NULL;
  if (startswith(e.type_name, "class "))
    {
      // normalize to match dwflpp::global_alias_caching_callback
      string struct_name = "struct " + e.type_name.substr(6);
      type_die = dw.declaration_resolve_other_cus(struct_name);
    }
  else
    type_die = dw.declaration_resolve_other_cus(e.type_name);

  // NB: We now index the types as "struct name"/"union name"/etc. instead of
  // just "name".  But since we didn't require users to be explicit before, and
  // actually sort of discouraged it, we must be flexible now.  So if a lookup
  // fails with a bare name, try augmenting it.
  if (!type_die &&
      !startswith(e.type_name, "class ") &&
      !startswith(e.type_name, "struct ") &&
      !startswith(e.type_name, "union ") &&
      !startswith(e.type_name, "enum "))
    {
      type_die = dw.declaration_resolve_other_cus("struct " + e.type_name);
      if (!type_die)
        type_die = dw.declaration_resolve_other_cus("union " + e.type_name);
      if (!type_die)
        type_die = dw.declaration_resolve_other_cus("enum " + e.type_name);
    }

  if (!type_die)
    return;

  string code;
  Dwarf_Die endtype;

  try
    {
      Dwarf_Die cu_mem;
      dw.focus_on_cu(dwarf_diecu(type_die, &cu_mem, NULL, NULL));

      if (e.check_pretty_print (lvalue))
        {
          dwarf_pretty_print dpp(dw, type_die, e.operand, true, userspace_p, e);
          result = dpp.expand();
          return;
        }

      code = dw.literal_stmt_for_pointer (type_die, &e, lvalue, &endtype);
    }
  catch (const semantic_error& er)
    {
      // NB: we can have multiple errors, since a @cast
      // may be attempted using several different modules:
      //     @cast(ptr, "type", "module1:module2:...")
      e.chain (er);
    }

  if (code.empty())
    return;

  string fname = (string(lvalue ? "_dwarf_cast_set" : "_dwarf_cast_get")
		  + "_" + e.sym_name()
		  + "_" + lex_cast(tick++));

  result = synthetic_embedded_deref_call(dw, &endtype, fname, code,
                                         userspace_p, lvalue, &e, e.operand);
}


void dwarf_cast_expanding_visitor::filter_special_modules(string& module)
{
  // look for "<path/to/header>" or "kernel<path/to/header>"
  // for those cases, build a module including that header
  if (module[module.size() - 1] == '>' &&
      (module[0] == '<' || startswith(module, "kernel<")))
    {
      string header = module;
      map<string,string>::const_iterator it = compiled_headers.find(header);
      if (it != compiled_headers.end())
        {
          module = it->second;
          return;
        }

      string cached_module;
      if (s.use_cache)
        {
          // see if the cached module exists
          cached_module = find_typequery_hash(s, module);
          if (!cached_module.empty() && !s.poison_cache)
            {
              int fd = open(cached_module.c_str(), O_RDONLY);
              if (fd != -1)
                {
                  if (s.verbose > 2)
                    //TRANSLATORS: Here we're using a cached module.
                    clog << _("Pass 2: using cached ") << cached_module << endl;
                  compiled_headers[header] = module = cached_module;
                  close(fd);
                  return;
                }
            }
        }

      // no cached module, time to make it
      if (make_typequery(s, module) == 0)
        {
          // try to save typequery in the cache
          if (s.use_cache)
            copy_file(module, cached_module, s.verbose > 2);
          compiled_headers[header] = module;
        }
    }
}


void dwarf_cast_expanding_visitor::visit_cast_op (cast_op* e)
{
  bool lvalue = is_active_lvalue(e);
  if (lvalue && !s.guru_mode)
    throw SEMANTIC_ERROR(_("write to @cast context variable not permitted; need stap -g"), e->tok);

  if (e->module.empty())
    e->module = "kernel"; // "*" may also be reasonable to search all kernel modules

  functioncall* result = NULL;

  // split the module string by ':' for alternatives
  vector<string> modules;
  tokenize(e->module, modules, ":");
  bool userspace_p=false; // PR10601
  for (unsigned i = 0; !result && i < modules.size(); ++i)
    {
      string& module = modules[i];
      filter_special_modules(module);

      // NB: This uses '/' to distinguish between kernel modules and userspace,
      // which means that userspace modules won't get any PATH searching.
      dwflpp* dw;
      try
	{
          userspace_p=is_user_module (module);
	  if (! userspace_p)
	    {
	      // kernel or kernel module target
	      dw = db.get_kern_dw(s, module);
	    }
	  else
	    {
              module = find_executable (module, "", s.sysenv); // canonicalize it
	      dw = db.get_user_dw(s, module);
	    }
	}
      catch (const semantic_error& er)
	{
	  /* ignore and go to the next module */
	  continue;
	}

      dwarf_cast_query q (*dw, module, *e, lvalue, userspace_p, result);
      dw->iterate_over_modules<base_query>(&query_module, &q);
    }

  if (!result)
    {
      // We pass the unresolved cast_op to the next pass, and hope
      // that this value ends up not being referenced after all, so
      // it can be optimized out quietly.
      provide (e);
      return;
    }

  if (lvalue)
    provide_lvalue_call (result);

  result->visit (this);
}


static bool resolve_pointer_type(Dwarf_Die& die, bool& isptr);

exp_type_dwarf::exp_type_dwarf(dwflpp* dw, Dwarf_Die* die,
                               bool userspace_p, bool addressof):
  dw(dw), die(*die), userspace_p(userspace_p), is_pointer(false)
{
  // is_pointer tells us whether a value is a pointer to the given type, so we
  // can dereference it; otherwise it will be treated as an end point.
  if (addressof)
    // we're already looking at the pointed-to type
    is_pointer = true;
  else
    // use the same test as tracepoints to see what we have
    resolve_pointer_type(this->die, is_pointer);
}


functioncall *
exp_type_dwarf::expand(autocast_op* e, bool lvalue)
{
  static unsigned tick = 0;

  try
    {
      // make sure we're not dereferencing base types or void
      bool deref_p = is_pointer && !null_die(&die);
      if (!deref_p)
        e->assert_no_components("autocast", true);

      if (lvalue && !dw->sess.guru_mode)
	throw SEMANTIC_ERROR(_("write not permitted; need stap -g"), e->tok);

      if (e->components.empty())
        {
          if (e->addressof)
            throw SEMANTIC_ERROR(_("cannot take address of tracepoint variable"), e->tok);

          // no components and no addressof?  how did this autocast come to be?
          throw SEMANTIC_ERROR(_("internal error: no-op autocast encountered"), e->tok);
        }

      Dwarf_Die cu_mem;
      if (!null_die(&die))
        dw->focus_on_cu(dwarf_diecu(&die, &cu_mem, NULL, NULL));

      if (e->check_pretty_print (lvalue))
	{
	  dwarf_pretty_print dpp(*dw, &die, e->operand, deref_p, userspace_p, *e);
	  return dpp.expand();
	}

      Dwarf_Die endtype;
      string code = dw->literal_stmt_for_pointer (&die, e, lvalue, &endtype);

      string fname = (string(lvalue ? "_dwarf_autocast_set" : "_dwarf_autocast_get")
		      + "_" + lex_cast(tick++));

      return synthetic_embedded_deref_call(*dw, &endtype, fname, code,
					  userspace_p, lvalue, e, e->operand);
    }
  catch (const semantic_error &er)
    {
      e->chain (er);
      return NULL;
    }
}


struct dwarf_atvar_expanding_visitor: public var_expanding_visitor
{
  systemtap_session& s;
  dwarf_builder& db;

  dwarf_atvar_expanding_visitor(systemtap_session& s, dwarf_builder& db):
    s(s), db(db) {}
  void visit_atvar_op (atvar_op* e);
};


struct dwarf_atvar_query: public base_query
{
  atvar_op& e;
  const bool userspace_p, lvalue;
  functioncall*& result;
  unsigned& tick;
  const string cu_name_pattern;

  dwarf_atvar_query(dwflpp& dw, const string& module, atvar_op& e,
                    const bool userspace_p, const bool lvalue,
                    functioncall*& result,
                    unsigned& tick):
    base_query(dw, module), e(e),
    userspace_p(userspace_p), lvalue(lvalue), result(result),
    tick(tick), cu_name_pattern(string("*/") + e.cu_name) {}

  void handle_query_module ();
  void query_library (const char *) {}
  void query_plt (const char *entry, size_t addr) {}
  static int atvar_query_cu (Dwarf_Die *cudie, dwarf_atvar_query *q);
};


int
dwarf_atvar_query::atvar_query_cu (Dwarf_Die * cudie, dwarf_atvar_query *q)
{
  if (! q->e.cu_name.empty())
    {
      const char *die_name = dwarf_diename(cudie) ?: "";
      if (strcmp(die_name, q->e.cu_name.c_str()) != 0 // Perfect match
          && fnmatch(q->cu_name_pattern.c_str(), die_name, 0) != 0)
        {
          return DWARF_CB_OK;
        }
    }

  try
    {
      vector<Dwarf_Die>  scopes(1, *cudie);

      q->dw.focus_on_cu (cudie);

      if (q->e.check_pretty_print (q->lvalue))
        {
          dwarf_pretty_print dpp (q->dw, scopes, 0, q->e.sym_name(),
                                  q->userspace_p, q->e);
          q->result = dpp.expand();
          return DWARF_CB_ABORT;
        }

      Dwarf_Die endtype;
      string code = q->dw.literal_stmt_for_local (scopes, 0, q->e.sym_name(),
                                                  &q->e, q->lvalue, &endtype);

      if (code.empty())
        return DWARF_CB_OK;

      string fname = (string(q->lvalue ? "_dwarf_tvar_set"
                                       : "_dwarf_tvar_get")
                      + "_" + q->e.sym_name()
                      + "_" + lex_cast(q->tick++));

      q->result = synthetic_embedded_deref_call (q->dw, &endtype, fname, code,
                                                 q->userspace_p, q->lvalue,
                                                 &q->e);
    }
  catch (const semantic_error& er)
    {
      // Here we suppress the error because we often just have too many
      // when scanning all the CUs.
      return DWARF_CB_OK;
    }

  if (q->result) {
      return DWARF_CB_ABORT;
  }

  return DWARF_CB_OK;
}


void
dwarf_atvar_query::handle_query_module ()
{

  dw.iterate_over_cus(atvar_query_cu, this, false);
}


void
dwarf_atvar_expanding_visitor::visit_atvar_op (atvar_op* e)
{
  const bool lvalue = is_active_lvalue(e);
  if (lvalue && !s.guru_mode)
    throw SEMANTIC_ERROR(_("write to @var variable not permitted; "
                           "need stap -g"), e->tok);

  if (e->module.empty())
    e->module = "kernel";

  functioncall* result = NULL;

  // split the module string by ':' for alternatives
  vector<string> modules;
  tokenize(e->module, modules, ":");
  bool userspace_p = false;
  for (unsigned i = 0; !result && i < modules.size(); ++i)
    {
      string& module = modules[i];

      dwflpp* dw;
      try
        {
          userspace_p = is_user_module(module);
          if (!userspace_p)
            {
              // kernel or kernel module target
              dw = db.get_kern_dw(s, module);
            }
          else
            {
              module = find_executable(module, "", s.sysenv);
              dw = db.get_user_dw(s, module);
            }
        }
      catch (const semantic_error& er)
        {
          /* ignore and go to the next module */
          continue;
        }

      dwarf_atvar_query q (*dw, module, *e, userspace_p, lvalue, result, tick);
      dw->iterate_over_modules<base_query>(&query_module, &q);

      if (result)
        {
          s.unwindsym_modules.insert(module);

          if (lvalue)
	    provide_lvalue_call (result);

          result->visit(this);
          return;
        }

      /* Unable to find the variable in the current module, so we chain
       * an error in atvar_op */
      semantic_error  er(ERR_SRC, _F("unable to find global '%s' in %s%s%s",
                            e->sym_name().c_str(), module.c_str(),
                            e->cu_name.empty() ? "" : _(", in "),
                            e->cu_name.c_str()));
      e->chain (er);
    }

  provide(e);
}


void
dwarf_derived_probe::printsig (ostream& o) const
{
  // Instead of just printing the plain locations, we add a PC value
  // as a comment as a way of telling e.g. apart multiple inlined
  // function instances.  This is distinct from the verbose/clog
  // output, since this part goes into the cache hash calculations.
  sole_location()->print (o);
  o << " /* pc=" << section << "+0x" << hex << addr << dec << " */";
  printsig_nested (o);
}



void
dwarf_derived_probe::join_group (systemtap_session& s)
{
  // skip probes which are paired entry-handlers
  if (!has_return && (saved_longs || saved_strings))
    return;

  if (! s.dwarf_derived_probes)
    s.dwarf_derived_probes = new dwarf_derived_probe_group ();
  s.dwarf_derived_probes->enroll (this);
  this->group = s.dwarf_derived_probes;
  if (has_return && entry_handler)
    entry_handler->group = s.dwarf_derived_probes;
}


static bool
kernel_supports_inode_uprobes(systemtap_session& s)
{
  // The arch-supports is new to the builtin inode-uprobes, so it makes a
  // reasonable indicator of the new API.  Else we'll need an autoconf...
  // see also buildrun.cxx:kernel_built_uprobs()
  return (s.kernel_config["CONFIG_ARCH_SUPPORTS_UPROBES"] == "y"
          && s.kernel_config["CONFIG_UPROBES"] == "y");
}


static bool
kernel_supports_inode_uretprobes(systemtap_session& s)
{
  // We need inode-uprobes first, then look for a sign of uretprobes.  The only
  // non-static function at present is arch_uretprobe_hijack_return_addr.
  return kernel_supports_inode_uprobes(s) &&
    (s.kernel_functions.count("arch_uretprobe_hijack_return_addr") > 0);
}


void
check_process_probe_kernel_support(systemtap_session& s)
{
  // If we've got utrace, we're good to go.
  if (s.kernel_config["CONFIG_UTRACE"] == "y")
    return;

  // We don't have utrace.  For process probes that aren't
  // uprobes-based, we just need the task_finder.  The task_finder
  // needs CONFIG_TRACEPOINTS and specific tracepoints.  There is a
  // specific autoconf test for its needs.
  //
  // We'll just require CONFIG_TRACEPOINTS here as a quick-and-dirty
  // approximation.
  if (! s.need_uprobes && s.kernel_config["CONFIG_TRACEPOINTS"] == "y")
    return;

  // For uprobes-based process probes, we need the task_finder plus
  // the builtin inode-uprobes.
  if (s.need_uprobes
      && s.kernel_config["CONFIG_TRACEPOINTS"] == "y"
      && kernel_supports_inode_uprobes(s))
    return;

  throw SEMANTIC_ERROR (_("process probes not available without kernel CONFIG_UTRACE or CONFIG_TRACEPOINTS/CONFIG_ARCH_SUPPORTS_UPROBES/CONFIG_UPROBES"));
}


dwarf_derived_probe::dwarf_derived_probe(const string& funcname,
                                         const string& filename,
                                         int line,
                                         // module & section specify a relocation
                                         // base for <addr>, unless section==""
                                         // (equivalently module=="kernel")
                                         // for userspace, it's a full path, for
                                         // modules, it's either a full path, or
                                         // the basename (e.g. 'btrfs')
                                         const string& module,
                                         const string& section,
                                         // NB: dwfl_addr is the virtualized
                                         // address for this symbol.
                                         Dwarf_Addr dwfl_addr,
                                         // addr is the section-offset for
                                         // actual relocation.
                                         Dwarf_Addr addr,
                                         dwarf_query& q,
                                         Dwarf_Die* scope_die /* may be null */)
  : derived_probe (q.base_probe, q.base_loc, true /* .components soon rewritten */ ),
    module (module), section (section), addr (addr),
    path (q.path),
    has_process (q.has_process),
    has_return (q.has_return),
    has_maxactive (q.has_maxactive),
    has_library (q.has_library),
    maxactive_val (q.maxactive_val),
    user_path (q.user_path),
    user_lib (q.user_lib),
    access_vars(false),
    saved_longs(0), saved_strings(0),
    entry_handler(0)
{
  // If we were given a fullpath to a kernel module, then get the simple name
  if (q.has_module && is_fully_resolved(module, q.dw.sess.sysroot, q.dw.sess.sysenv))
    this->module = modname_from_path(module);

  if (user_lib.size() != 0)
    has_library = true;

  if (q.has_process)
    {
      // We may receive probes on two types of ELF objects: ET_EXEC or ET_DYN.
      // ET_EXEC ones need no further relocation on the addr(==dwfl_addr), whereas
      // ET_DYN ones do (addr += run-time mmap base address).  We tell these apart
      // by the incoming section value (".absolute" vs. ".dynamic").
      // XXX Assert invariants here too?

      // inode-uprobes needs an offset rather than an absolute VM address.
      // ditto for userspace runtimes (dyninst)
      if ((kernel_supports_inode_uprobes(q.dw.sess) || q.dw.sess.runtime_usermode_p()) &&
          section == ".absolute" && addr == dwfl_addr &&
          addr >= q.dw.module_start && addr < q.dw.module_end)
        this->addr = addr - q.dw.module_start;
    }
  else
    {
      // Assert kernel relocation invariants
      if (section == "" && dwfl_addr != addr) // addr should be absolute
        throw SEMANTIC_ERROR (_("missing relocation basis"), tok);
      if (section != "" && dwfl_addr == addr) // addr should be an offset
        throw SEMANTIC_ERROR (_("inconsistent relocation address"), tok);
    }

  // XXX: hack for strange g++/gcc's
#ifndef USHRT_MAX
#define USHRT_MAX 32767
#endif

  // Range limit maxactive() value
  if (has_maxactive && (maxactive_val < 0 || maxactive_val > USHRT_MAX))
    throw SEMANTIC_ERROR (_F("maxactive value out of range [0,%s]",
                          lex_cast(USHRT_MAX).c_str()), q.base_loc->components.front()->tok);

  // Expand target variables in the probe body. Even if the scope_die is
  // invalid, we still want to expand things such as $$vars/$$parms/etc...
  // (PR15999, PR16473). Access to specific context vars e.g. $argc will not be
  // expanded and will produce an error during the typeresolution_info pass.
    {
      // XXX: user-space deref's for q.has_process!

      // PR14436: if we're expanding target variables in the probe body of a
      // .return probe, we need to make the expansion at the postprologue addr
      // instead (if any), which is then also the spot where the entry handler
      // probe is placed. (Note that at this point, a nonzero prologue_end
      // implies that it should be used, i.e. code is unoptimized).
      Dwarf_Addr handler_dwfl_addr = dwfl_addr;
      if (q.prologue_end != 0 && q.has_return)
        {
          handler_dwfl_addr = q.prologue_end;
          if (q.sess.verbose > 2)
            clog << _F("expanding .return vars at prologue_end (0x%s) "
                       "rather than entrypc (0x%s)\n",
                       lex_cast_hex(handler_dwfl_addr).c_str(),
                       lex_cast_hex(dwfl_addr).c_str());
        }
      dwarf_var_expanding_visitor v (q, scope_die, handler_dwfl_addr);
      v.replace (this->body);

      // Propagate perf.counters so we can emit later
      this->perf_counter_refs = v.perf_counter_refs;
      // Emit local var used to save the perf counter read value
      std::set<string>::iterator pcii;
      for (pcii = v.perf_counter_refs.begin();
	   pcii != v.perf_counter_refs.end(); pcii++)
	{
	  std::vector<std::pair<std::string,std::string> >::iterator it;
	  // Find the associated perf counter probe
	  for (it=q.sess.perf_counters.begin() ;
	       it != q.sess.perf_counters.end();
	       it++)
	    if ((*it).first == (*pcii))
              {
                vardecl* vd = new vardecl;
                vd->name = "__perf_read_" + (*it).first;
                vd->tok = this->tok;
                vd->set_arity(0, this->tok);
                vd->type = pe_long;
                vd->synthetic = true;
                this->locals.push_back (vd);
                break;
              }
	}


      if (!q.has_process)
        access_vars = v.visited;

      // If during target-variable-expanding the probe, we added a new block
      // of code, add it to the start of the probe.
      if (v.add_block)
        this->body = new block(v.add_block, this->body);

      // If when target-variable-expanding the probe, we need to synthesize a
      // sibling function-entry probe.  We don't go through the whole probe derivation
      // business (PR10642) that could lead to wildcard/alias resolution, or for that
      // dwarf-induced duplication.
      if (v.add_call_probe)
        {
          assert (q.has_return && !q.has_call);

          // We temporarily replace q.base_probe.
          statement* old_body = q.base_probe->body;
          q.base_probe->body = v.add_call_probe;
          q.has_return = false;
          q.has_call = true;

          if (q.has_process)
            {
              // Place handler probe at the same addr as where the vars were
              // expanded (which may not be the same addr as the one for the
              // main retprobe, PR14436).
              Dwarf_Addr handler_addr = addr;
              if (handler_dwfl_addr != dwfl_addr)
                // adjust section offset by prologue_end-entrypc
                handler_addr += handler_dwfl_addr - dwfl_addr;
              entry_handler = new uprobe_derived_probe (funcname, filename,
                                                        line, module, section,
                                                        handler_dwfl_addr,
                                                        handler_addr, q,
                                                        scope_die);
            }
          else
            entry_handler = new dwarf_derived_probe (funcname, filename, line,
                                                     module, section, dwfl_addr,
                                                     addr, q, scope_die);

          saved_longs = entry_handler->saved_longs = v.saved_longs;
          saved_strings = entry_handler->saved_strings = v.saved_strings;

          q.results.push_back (entry_handler);

          q.has_return = true;
          q.has_call = false;
          q.base_probe->body = old_body;
        }
      // Save the local variables for listing mode. If the scope_die is null,
      // local vars aren't accessible, so no need to invoke saveargs (PR10820).
      if (!null_die(scope_die) &&
          q.sess.dump_mode == systemtap_session::dump_matched_probes_vars)
        saveargs(q, scope_die, dwfl_addr);
    }

  // Reset the sole element of the "locations" vector as a
  // "reverse-engineered" form of the incoming (q.base_loc) probe
  // point.  This allows a user to see what function / file / line
  // number any particular match of the wildcards.

  vector<probe_point::component*> comps;
  if (q.has_kernel)
    comps.push_back (new probe_point::component(TOK_KERNEL));
  else if(q.has_module)
    comps.push_back (new probe_point::component(TOK_MODULE, new literal_string(module)));
  else if(q.has_process)
    comps.push_back (new probe_point::component(TOK_PROCESS, new literal_string(module)));
  else
    assert (0);

  string fn_or_stmt;
  if (q.has_function_str || q.has_function_num)
    fn_or_stmt = TOK_FUNCTION;
  else
    fn_or_stmt = TOK_STATEMENT;

  if (q.has_function_str || q.has_statement_str)
      {
        string retro_name = q.final_function_name(funcname, filename.c_str(), line);
        comps.push_back
          (new probe_point::component
           (fn_or_stmt, new literal_string (retro_name)));
      }
  else if (q.has_function_num || q.has_statement_num)
    {
      Dwarf_Addr retro_addr;
      if (q.has_function_num)
        retro_addr = q.function_num_val;
      else
        retro_addr = q.statement_num_val;
      comps.push_back (new probe_point::component
                       (fn_or_stmt,
                        new literal_number(retro_addr, true)));

      if (q.has_absolute)
        comps.push_back (new probe_point::component (TOK_ABSOLUTE));
    }

  if (q.has_call)
      comps.push_back (new probe_point::component(TOK_CALL));
  if (q.has_exported)
      comps.push_back (new probe_point::component(TOK_EXPORTED));
  if (q.has_inline)
      comps.push_back (new probe_point::component(TOK_INLINE));
  if (has_return)
    comps.push_back (new probe_point::component(TOK_RETURN));
  if (has_maxactive)
    comps.push_back (new probe_point::component
                     (TOK_MAXACTIVE, new literal_number(maxactive_val)));

  // Overwrite it.
  this->sole_location()->components = comps;

  // if it's a .callee[s[(N)]] call, add checks to the probe body so that the
  // user body is only 'triggered' when called from q.callers[N-1], which
  // itself is called from q.callers[N-2], etc... I.E.
  // callees(N) --> N elements in q.callers --> N checks against [u]stack(0..N-1)
  if ((q.has_callee || q.has_callees_num) && q.callers && !q.callers->empty())
    {
      if (q.sess.verbose > 2)
        clog << _F("adding caller checks for callee %s\n", funcname.c_str());

      // Copy the stack and empty it out
      stack<Dwarf_Addr> callers(*q.callers);
      for (unsigned level = 1; !callers.empty(); level++,
                                                 callers.pop())
        {
          Dwarf_Addr caller = callers.top();

          // We first need to make the caller addr relocatable
          string caller_section;
          Dwarf_Addr caller_reloc;
          if (module == TOK_KERNEL)
            { // allow for relocatable kernel (see also add_probe_point())
              caller_reloc = caller - q.sess.sym_stext;
              caller_section = "_stext";
            }
          else
            caller_reloc = q.dw.relocate_address(caller,
                                                 caller_section);

          if (q.sess.verbose > 2)
            clog << _F("adding caller check [u]stack(%d) == reloc(0x%s)\n",
                       level, lex_cast_hex(caller_reloc).c_str());

          // We want to add a statement like this:
          // if (!_caller_match(user, mod, sec, addr)) next;
          // Something similar is done in semantic_pass_conditions()

          functioncall* check = new functioncall();
          check->tok = this->tok;
          check->function = "_caller_match";
          check->args.push_back(new literal_number(q.has_process));
          check->args[0]->tok = this->tok;
          // For callee .return probes, the callee is popped off stack
          // so we don't want to match the frame below the caller
          if (q.has_return)
            check->args.push_back(new literal_number(level-1));
          else
            check->args.push_back(new literal_number(level));
          check->args[1]->tok = this->tok;
          check->args.push_back(new literal_string(this->module));
          check->args[2]->tok = this->tok;
          check->args.push_back(new literal_string(caller_section));
          check->args[3]->tok = this->tok;
          check->args.push_back(new literal_number(caller_reloc, true /* hex */));
          check->args[4]->tok = this->tok;

          unary_expression* notexp = new unary_expression();
          notexp->tok = this->tok;
          notexp->op = "!";
          notexp->operand = check;

          if_statement* ifs = new if_statement();
          ifs->tok = this->tok;
          ifs->thenblock = new next_statement();
          ifs->thenblock->tok = this->tok;
          ifs->elseblock = NULL;
          ifs->condition = notexp;

          this->body = new block(ifs, this->body);
        }
    }
}


void
dwarf_derived_probe::saveargs(dwarf_query& q, Dwarf_Die* scope_die,
                              Dwarf_Addr dwfl_addr)
{
  if (null_die(scope_die))
    return;

  bool verbose = q.sess.verbose > 2;

  if (verbose)
    clog << _F("saveargs: examining '%s' (dieoffset: %#" PRIx64 ")\n", (dwarf_diename(scope_die)?: "unknown"), dwarf_dieoffset(scope_die));

  if (has_return)
    {
      /* Only save the return value if it has a type. */
      string type_name;
      Dwarf_Die type_die;
      if (dwarf_attr_die (scope_die, DW_AT_type, &type_die) &&
          dwarf_type_name(&type_die, type_name))
        args.push_back("$return:"+type_name);

      else if (verbose)
        clog << _F("saveargs: failed to retrieve type name for return value (dieoffset: %s)\n",
                   lex_cast_hex(dwarf_dieoffset(scope_die)).c_str());
    }

  Dwarf_Die arg;
  vector<Dwarf_Die> scopes = q.dw.getscopes(scope_die);
  for (unsigned i = 0; i < scopes.size(); ++i)
    {
      if (dwarf_tag(&scopes[i]) == DW_TAG_compile_unit)
        break; // we don't want file-level variables
      if (dwarf_child (&scopes[i], &arg) == 0)
        do
          {
            switch (dwarf_tag (&arg))
              {
              case DW_TAG_variable:
              case DW_TAG_formal_parameter:
                break;

              default:
                continue;
              }

            /* Ignore this local if it has no name. */
            const char *arg_name = dwarf_diename (&arg);
            if (!arg_name)
              {
                if (verbose)
                  clog << _F("saveargs: failed to retrieve name for local (dieoffset: %s)\n",
                             lex_cast_hex(dwarf_dieoffset(&arg)).c_str());
                continue;
              }

            if (verbose)
              clog << _F("saveargs: finding location for local '%s' (dieoffset: %s)\n",
                         arg_name, lex_cast_hex(dwarf_dieoffset(&arg)).c_str());

            /* Ignore this local if it has no location (or not at this PC). */
            /* NB: It still may not be directly accessible, e.g. if it is an
             * aggregate type, implicit_pointer, etc., but the user can later
             * figure out how to access the interesting parts. */

            /* XXX: Perhaps saveargs() / listings-mode should work by synthesizing
             * several synthetic
             *     probe foo { $var }
             * probes, testing them for overall resolvability.
             */

            Dwarf_Attribute attr_mem;
            if (!dwarf_attr_integrate (&arg, DW_AT_const_value, &attr_mem))
              {
                Dwarf_Op *expr;
                size_t len;
                if (!dwarf_attr_integrate (&arg, DW_AT_location, &attr_mem))
                  {
                    if (verbose)
                      clog << _F("saveargs: failed to resolve the location for local '%s' (dieoffset: %s)\n",
                                  arg_name, lex_cast_hex(dwarf_dieoffset(&arg)).c_str());
                    continue;
                  }
                else if (!(dwarf_getlocation_addr(&attr_mem, dwfl_addr, &expr,
                                                  &len, 1) == 1 && len > 0))
                  {
                    Dwarf_Addr dwfl_addr2 = q.dw.pr15123_retry_addr (dwfl_addr, & arg);
                    if (!dwfl_addr2 || (!(dwarf_getlocation_addr(&attr_mem, dwfl_addr2, &expr,
                                                                 &len, 1) == 1 && len > 0))) {
                      if (verbose)
                        clog << _F("saveargs: local '%s' (dieoffset: %s) is not available at this address (%s)\n",
                                   arg_name, lex_cast_hex(dwarf_dieoffset(&arg)).c_str(), lex_cast_hex(dwfl_addr).c_str());
                      continue;
                    }
                  }
              }

            /* Ignore this local if it has no type. */
            string type_name;
            Dwarf_Die type_die;
            if (!dwarf_attr_die (&arg, DW_AT_type, &type_die) ||
                !dwarf_type_name(&type_die, type_name))
              {
                if (verbose)
                  clog << _F("saveargs: failed to retrieve type name for local '%s' (dieoffset: %s)\n",
                             arg_name, lex_cast_hex(dwarf_dieoffset(&arg)).c_str());
                continue;
              }

            /* This local looks good -- save it! */
            args.push_back("$"+string(arg_name)+":"+type_name);
          }
        while (dwarf_siblingof (&arg, &arg) == 0);
    }
}


void
dwarf_derived_probe::getargs(std::list<std::string> &arg_set) const
{
  arg_set.insert(arg_set.end(), args.begin(), args.end());
}


void
dwarf_derived_probe::emit_privilege_assertion (translator_output* o)
{
  if (has_process)
    {
      // These probes are allowed for unprivileged users, but only in the
      // context of processes which they own.
      emit_process_owner_assertion (o);
      return;
    }

  // Other probes must contain the default assertion which aborts
  // if executed by an unprivileged user.
  derived_probe::emit_privilege_assertion (o);
}


void
dwarf_derived_probe::print_dupe_stamp(ostream& o)
{
  if (has_process)
    {
      // These probes are allowed for unprivileged users, but only in the
      // context of processes which they own.
      print_dupe_stamp_unprivileged_process_owner (o);
      return;
    }

  // Other probes must contain the default dupe stamp
  derived_probe::print_dupe_stamp (o);
}


void
dwarf_derived_probe::register_statement_variants(match_node * root,
						 dwarf_builder * dw,
						 privilege_t privilege)
{
  root
    ->bind_privilege(privilege)
    ->bind(dw);
  root->bind(TOK_NEAREST)
    ->bind_privilege(privilege)
    ->bind(dw);
}

void
dwarf_derived_probe::register_function_variants(match_node * root,
						dwarf_builder * dw,
						privilege_t privilege)
{
  root
    ->bind_privilege(privilege)
    ->bind(dw);
  root->bind(TOK_CALL)
    ->bind_privilege(privilege)
    ->bind(dw);
  root->bind(TOK_EXPORTED)
    ->bind_privilege(privilege)
    ->bind(dw);
  root->bind(TOK_RETURN)
    ->bind_privilege(privilege)
    ->bind(dw);

  // For process probes / uprobes, .maxactive() is unused.
  if (! pr_contains (privilege, pr_stapusr))
    {
      root->bind(TOK_RETURN)
        ->bind_num(TOK_MAXACTIVE)->bind(dw);
    }
}

void
dwarf_derived_probe::register_function_and_statement_variants(
  systemtap_session& s,
  match_node * root,
  dwarf_builder * dw,
  privilege_t privilege
)
{
  // Here we match 4 forms:
  //
  // .function("foo")
  // .function(0xdeadbeef)
  // .statement("foo")
  // .statement(0xdeadbeef)

  match_node *fv_root = root->bind_str(TOK_FUNCTION);
  register_function_variants(fv_root, dw, privilege);
  // ROOT.function("STRING") always gets the .inline and .label variants.
  fv_root->bind(TOK_INLINE)
    ->bind_privilege(privilege)
    ->bind(dw);
  fv_root->bind_str(TOK_LABEL)
    ->bind_privilege(privilege)
    ->bind(dw);
  fv_root->bind_str(TOK_CALLEE)
    ->bind_privilege(privilege)
    ->bind(dw);
  fv_root->bind_str(TOK_CALLEE)
    ->bind(TOK_RETURN)
    ->bind_privilege(privilege)
    ->bind(dw);
  fv_root->bind_str(TOK_CALLEE)
    ->bind(TOK_CALL)
    ->bind_privilege(privilege)
    ->bind(dw);
  fv_root->bind(TOK_CALLEES)
    ->bind_privilege(privilege)
    ->bind(dw);
  fv_root->bind_num(TOK_CALLEES)
    ->bind_privilege(privilege)
    ->bind(dw);

  fv_root = root->bind_num(TOK_FUNCTION);
  register_function_variants(fv_root, dw, privilege);
  // ROOT.function(NUMBER).inline is deprecated in release 1.7 and removed thereafter.
  if (strverscmp(s.compatible.c_str(), "1.7") <= 0)
    {
      fv_root->bind(TOK_INLINE)
	->bind_privilege(privilege)
	->bind(dw);
    }

  register_statement_variants(root->bind_str(TOK_STATEMENT), dw, privilege);
  register_statement_variants(root->bind_num(TOK_STATEMENT), dw, privilege);
}

void
dwarf_derived_probe::register_sdt_variants(systemtap_session& s,
					   match_node * root,
					   dwarf_builder * dw)
{
  root->bind_str(TOK_MARK)
    ->bind_privilege(pr_all)
    ->bind(dw);
  root->bind_str(TOK_PROVIDER)->bind_str(TOK_MARK)
    ->bind_privilege(pr_all)
    ->bind(dw);
}

void
dwarf_derived_probe::register_plt_variants(systemtap_session& s,
					   match_node * root,
					   dwarf_builder * dw)
{
  root->bind(TOK_PLT)
    ->bind_privilege(pr_all)
    ->bind(dw);
  root->bind_str(TOK_PLT)
    ->bind_privilege(pr_all)
    ->bind(dw);

  root->bind(TOK_PLT)
    ->bind(TOK_RETURN)
    ->bind_privilege(pr_all)
    ->bind(dw);
  root->bind_str(TOK_PLT)
    ->bind(TOK_RETURN)
    ->bind_privilege(pr_all)
    ->bind(dw);
}

void
dwarf_derived_probe::register_patterns(systemtap_session& s)
{
  match_node* root = s.pattern_root;
  dwarf_builder *dw = new dwarf_builder();

  update_visitor *filter = new dwarf_cast_expanding_visitor(s, *dw);
  s.code_filters.push_back(filter);

  filter = new dwarf_atvar_expanding_visitor(s, *dw);
  s.code_filters.push_back(filter);

  register_function_and_statement_variants(s, root->bind(TOK_KERNEL), dw, pr_privileged);
  register_function_and_statement_variants(s, root->bind_str(TOK_MODULE), dw, pr_privileged);
  root->bind(TOK_KERNEL)->bind_num(TOK_STATEMENT)->bind(TOK_ABSOLUTE)
    ->bind(dw);

  match_node* uprobes[] = {
      root->bind(TOK_PROCESS),
      root->bind_str(TOK_PROCESS),
      root->bind_num(TOK_PROCESS),
      root->bind(TOK_PROCESS)->bind_str(TOK_LIBRARY),
      root->bind_str(TOK_PROCESS)->bind_str(TOK_LIBRARY),
  };
  for (size_t i = 0; i < sizeof(uprobes) / sizeof(*uprobes); ++i)
    {
      register_function_and_statement_variants(s, uprobes[i], dw, pr_all);
      register_sdt_variants(s, uprobes[i], dw);
      register_plt_variants(s, uprobes[i], dw);
    }
}

void
dwarf_derived_probe::emit_probe_local_init(systemtap_session& s, translator_output * o)
{
  std::set<string>::iterator pcii;
  for (pcii = perf_counter_refs.begin();
       pcii != perf_counter_refs.end();
       pcii++)
    {
      std::vector<std::pair<std::string,std::string> >::iterator it;
      // Find the associated perf.counter probe
      unsigned i = 0;
      for (it=s.perf_counters.begin() ;
	   it != s.perf_counters.end();
	   it++, i++)
	if ((*it).first == (*pcii))
          {
            // place the perf counter read so it precedes stp_lock_probe
            o->newline() << "l->l___perf_read_" + (*it).first
              + " = (((int64_t) (_stp_perf_read(smp_processor_id(),"
              + lex_cast(i) + "))));";
            break;
          }
    }

  if (access_vars)
    {
      // if accessing $variables, emit bsp cache setup for speeding up
      o->newline() << "#if defined __ia64__";
      o->newline() << "bspcache(c->unwaddr, c->kregs);";
      o->newline() << "#endif";
    }
}

// ------------------------------------------------------------------------

void
dwarf_derived_probe_group::enroll (dwarf_derived_probe* p)
{
  probes_by_module.insert (make_pair (p->module, p));

  // XXX: probes put at the same address should all share a
  // single kprobe/kretprobe, and have their handlers executed
  // sequentially.
}

void
dwarf_derived_probe_group::emit_module_decls (systemtap_session& s)
{
  if (probes_by_module.empty()) return;

  s.op->newline() << "/* ---- dwarf probes ---- */";

  // Let's find some stats for the embedded strings.  Maybe they
  // are small and uniform enough to justify putting char[MAX]'s into
  // the array instead of relocated char*'s.
  size_t module_name_max = 0, section_name_max = 0;
  size_t module_name_tot = 0, section_name_tot = 0;
  size_t all_name_cnt = probes_by_module.size(); // for average
  for (p_b_m_iterator it = probes_by_module.begin(); it != probes_by_module.end(); it++)
    {
      dwarf_derived_probe* p = it->second;
#define DOIT(var,expr) do {                             \
        size_t var##_size = (expr) + 1;                 \
        var##_max = max (var##_max, var##_size);        \
        var##_tot += var##_size; } while (0)
      DOIT(module_name, p->module.size());
      DOIT(section_name, p->section.size());
#undef DOIT
    }

  // Decide whether it's worthwhile to use char[] or char* by comparing
  // the amount of average waste (max - avg) to the relocation data size
  // (3 native long words).
#define CALCIT(var)                                                     \
  if ((var##_name_max-(var##_name_tot/all_name_cnt)) < (3 * sizeof(void*))) \
    {                                                                   \
      s.op->newline() << "#define STAP_DWARF_PROBE_STR_" << #var << " " \
                      << "const char " << #var                          \
                      << "[" << var##_name_max << "]";                 \
      if (s.verbose > 2) clog << "stap_dwarf_probe " << #var            \
                              << "[" << var##_name_max << "]" << endl;  \
    }                                                                   \
  else                                                                  \
    {                                                                   \
      s.op->newline() << "#define STAP_DWARF_PROBE_STR_" << #var << " " \
                      << "const char * const " << #var << "";          \
      if (s.verbose > 2) clog << "stap_dwarf_probe *" << #var << endl;  \
    }

  CALCIT(module);
  CALCIT(section);

#undef CALCIT

  s.op->newline() << "#include \"linux/kprobes.c\"";

#define UNDEFIT(var) s.op->newline() << "#undef STAP_DWARF_PROBE_STR_" << #var
  UNDEFIT(module);
  UNDEFIT(section);
#undef UNDEFIT

  // Emit an array of kprobe/kretprobe pointers
  s.op->newline() << "#if defined(STAPCONF_UNREGISTER_KPROBES)";
  s.op->newline() << "static void * stap_unreg_kprobes[" << probes_by_module.size() << "];";
  s.op->newline() << "#endif";

  // Emit the actual probe list.

  // NB: we used to plop a union { struct kprobe; struct kretprobe } into
  // struct stap_dwarf_probe, but it being initialized data makes it add
  // hundreds of bytes of padding per stap_dwarf_probe.  (PR5673)
  s.op->newline() << "static struct stap_dwarf_kprobe stap_dwarf_kprobes[" << probes_by_module.size() << "];";
  // NB: bss!

  s.op->newline() << "static struct stap_dwarf_probe stap_dwarf_probes[] = {";
  s.op->indent(1);

  size_t stap_dwarf_kprobe_idx = 0;
  for (p_b_m_iterator it = probes_by_module.begin(); it != probes_by_module.end(); it++)
    {
      dwarf_derived_probe* p = it->second;
      s.op->newline() << "{";
      if (p->has_return)
        s.op->line() << " .return_p=1,";
      if (p->has_maxactive)
        {
          s.op->line() << " .maxactive_p=1,";
          assert (p->maxactive_val >= 0 && p->maxactive_val <= USHRT_MAX);
          s.op->line() << " .maxactive_val=" << p->maxactive_val << ",";
        }
      if (p->saved_longs || p->saved_strings)
        {
          if (p->saved_longs)
            s.op->line() << " .saved_longs=" << p->saved_longs << ",";
          if (p->saved_strings)
            s.op->line() << " .saved_strings=" << p->saved_strings << ",";
          if (p->entry_handler)
            s.op->line() << " .entry_probe=" << common_probe_init (p->entry_handler) << ",";
        }
      if (p->locations[0]->optional)
        s.op->line() << " .optional_p=1,";
      s.op->line() << " .address=(unsigned long)0x" << hex << p->addr << dec << "ULL,";
      s.op->line() << " .module=\"" << p->module << "\",";
      s.op->line() << " .section=\"" << p->section << "\",";
      s.op->line() << " .probe=" << common_probe_init (p) << ",";
      s.op->line() << " .kprobe=&stap_dwarf_kprobes[" << stap_dwarf_kprobe_idx++ << "],";
      s.op->line() << " },";
    }

  s.op->newline(-1) << "};";

  // Emit the kprobes callback function
  s.op->newline();
  s.op->newline() << "static int enter_kprobe_probe (struct kprobe *inst,";
  s.op->line() << " struct pt_regs *regs) {";
  // NB: as of PR5673, the kprobe|kretprobe union struct is in BSS
  s.op->newline(1) << "int kprobe_idx = ((uintptr_t)inst-(uintptr_t)stap_dwarf_kprobes)/sizeof(struct stap_dwarf_kprobe);";
  // Check that the index is plausible
  s.op->newline() << "struct stap_dwarf_probe *sdp = &stap_dwarf_probes[";
  s.op->line() << "((kprobe_idx >= 0 && kprobe_idx < " << probes_by_module.size() << ")?";
  s.op->line() << "kprobe_idx:0)"; // NB: at least we avoid memory corruption
  // XXX: it would be nice to give a more verbose error though; BUG_ON later?
  s.op->line() << "];";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sdp->probe",
				 "stp_probe_type_kprobe");
  s.op->newline() << "c->kregs = regs;";

  // Make it look like the IP is set as it wouldn't have been replaced
  // by a breakpoint instruction when calling real probe handler. Reset
  // IP regs on return, so we don't confuse kprobes. PR10458
  s.op->newline() << "{";
  s.op->indent(1);
  s.op->newline() << "unsigned long kprobes_ip = REG_IP(c->kregs);";
  s.op->newline() << "SET_REG_IP(regs, (unsigned long) inst->addr);";
  s.op->newline() << "(*sdp->probe->ph) (c);";
  s.op->newline() << "SET_REG_IP(regs, kprobes_ip);";
  s.op->newline(-1) << "}";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";

  // Same for kretprobes
  s.op->newline();
  s.op->newline() << "static int enter_kretprobe_common (struct kretprobe_instance *inst,";
  s.op->line() << " struct pt_regs *regs, int entry) {";
  s.op->newline(1) << "struct kretprobe *krp = inst->rp;";

  // NB: as of PR5673, the kprobe|kretprobe union struct is in BSS
  s.op->newline() << "int kprobe_idx = ((uintptr_t)krp-(uintptr_t)stap_dwarf_kprobes)/sizeof(struct stap_dwarf_kprobe);";
  // Check that the index is plausible
  s.op->newline() << "struct stap_dwarf_probe *sdp = &stap_dwarf_probes[";
  s.op->line() << "((kprobe_idx >= 0 && kprobe_idx < " << probes_by_module.size() << ")?";
  s.op->line() << "kprobe_idx:0)"; // NB: at least we avoid memory corruption
  // XXX: it would be nice to give a more verbose error though; BUG_ON later?
  s.op->line() << "];";

  s.op->newline() << "const struct stap_probe *sp = entry ? sdp->entry_probe : sdp->probe;";
  s.op->newline() << "if (sp) {";
  s.op->indent(1);
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sp",
				 "stp_probe_type_kretprobe");
  s.op->newline() << "c->kregs = regs;";

  // for assisting runtime's backtrace logic and accessing kretprobe data packets
  s.op->newline() << "c->ips.krp.pi = inst;";
  s.op->newline() << "c->ips.krp.pi_longs = sdp->saved_longs;";

  // Make it look like the IP is set as it wouldn't have been replaced
  // by a breakpoint instruction when calling real probe handler. Reset
  // IP regs on return, so we don't confuse kprobes. PR10458
  s.op->newline() << "{";
  s.op->newline(1) << "unsigned long kprobes_ip = REG_IP(c->kregs);";
  s.op->newline() << "if (entry)";
  s.op->newline(1) << "SET_REG_IP(regs, (unsigned long) inst->rp->kp.addr);";
  s.op->newline(-1) << "else";
  s.op->newline(1) << "SET_REG_IP(regs, (unsigned long)inst->ret_addr);";
  s.op->newline(-1) << "(sp->ph) (c);";
  s.op->newline() << "SET_REG_IP(regs, kprobes_ip);";
  s.op->newline(-1) << "}";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline(-1) << "}";
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";

  s.op->newline();
}


void
dwarf_derived_probe_group::emit_module_init (systemtap_session& s)
{
  if (probes_by_module.empty()) return;

  s.op->newline() << "/* ---- dwarf probes ---- */";

  // We'll let stapkp_init() handle reporting errors by setting probe_point to
  // NULL.
  s.op->newline() << "probe_point = NULL;";

  s.op->newline() << "rc = stapkp_init( "
                                     << "stap_dwarf_probes, "
                                     << "ARRAY_SIZE(stap_dwarf_probes));";
}


void
dwarf_derived_probe_group::emit_module_refresh (systemtap_session& s)
{
  if (probes_by_module.empty()) return;

  s.op->newline() << "/* ---- dwarf probes ---- */";

  s.op->newline() << "stapkp_refresh( "
                                   << "modname, "
                                   << "stap_dwarf_probes, "
                                   << "ARRAY_SIZE(stap_dwarf_probes));";
}


void
dwarf_derived_probe_group::emit_module_exit (systemtap_session& s)
{
  if (probes_by_module.empty()) return;

  s.op->newline() << "/* ---- dwarf probes ---- */";

  s.op->newline() << "stapkp_exit( "
                                << "stap_dwarf_probes, "
                                << "ARRAY_SIZE(stap_dwarf_probes));";
}

static void sdt_v3_tokenize(const string& str, vector<string>& tokens)
{
  string::size_type pos;
  string::size_type lastPos = str.find_first_not_of(" ", 0);
  string::size_type nextAt = str.find("@", lastPos);

  if (nextAt == string::npos)
    {
      // PR13934: Assembly probes are not forced to use the N@OP form.
      // In this case, N is inferred to be the native word size.  Since we
      // don't have a nice delimiter, just split it on spaces.  SDT-asm authors
      // then must not put any spaces in arguments, to avoid ambiguity.
      tokenize(str, tokens, " ");
      return;
    }

  while (lastPos != string::npos)
   {
     pos = nextAt + 1;
     nextAt = str.find("@", pos);
     if (nextAt == string::npos)
       pos = string::npos;
     else
       pos = str.rfind(" ", nextAt);

     tokens.push_back(str.substr(lastPos, pos - lastPos));
     lastPos = str.find_first_not_of(" ", pos);
   }
}


struct sdt_uprobe_var_expanding_visitor: public var_expanding_visitor
{
  enum regwidths {QI, QIh, HI, SI, DI};
  sdt_uprobe_var_expanding_visitor(systemtap_session& s,
                                   dwflpp& dw,
                                   int elf_machine,
                                   const string & process_name,
				   const string & provider_name,
				   const string & probe_name,
				   stap_sdt_probe_type probe_type,
				   const string & arg_string,
				   int ac):
    session (s), dw (dw), elf_machine (elf_machine),
    process_name (process_name), provider_name (provider_name),
    probe_name (probe_name), probe_type (probe_type), arg_count ((unsigned) ac)
  {
    // sanity check that we're not somehow here for a kernel probe
    assert(is_user_module(process_name));

    build_dwarf_registers();

    need_debug_info = false;
    if (probe_type == uprobe3_type)
      {
        sdt_v3_tokenize(arg_string, arg_tokens);
        assert(arg_count <= 12);
      }
    else
      {
        tokenize(arg_string, arg_tokens, " ");
        assert(arg_count <= 10);
      }
  }

  systemtap_session& session;
  dwflpp& dw;
  int elf_machine;
  const string & process_name;
  const string & provider_name;
  const string & probe_name;
  stap_sdt_probe_type probe_type;
  unsigned arg_count;
  vector<string> arg_tokens;

  map<string, pair<unsigned,int> > dwarf_regs;
  string regnames;
  string percent_regnames;

  bool need_debug_info;

  void build_dwarf_registers();
  void visit_target_symbol (target_symbol* e);
  unsigned get_target_symbol_argno_and_validate (target_symbol* e);
  long parse_out_arg_precision(string& asmarg);
  expression* try_parse_arg_literal (target_symbol *e,
                                     const string& asmarg,
                                     long precision);
  expression* try_parse_arg_register (target_symbol *e,
                                      const string& asmarg,
                                      long precision);
  expression* try_parse_arg_offset_register (target_symbol *e,
                                             const string& asmarg,
                                             long precision);
  expression* try_parse_arg_effective_addr (target_symbol *e,
                                            const string& asmarg,
                                            long precision);
  expression* try_parse_arg_varname (target_symbol *e,
                                     const string& asmarg,
                                     long precision);
  void visit_target_symbol_arg (target_symbol* e);
  void visit_target_symbol_context (target_symbol* e);
  void visit_atvar_op (atvar_op* e);
  void visit_cast_op (cast_op* e);
};

void
sdt_uprobe_var_expanding_visitor::build_dwarf_registers ()
{
  /* Register name mapping table depends on the elf machine of this particular
     probe target process/file, not upon the host.  So we can't just
     #ifdef _i686_ etc. */

#define DRI(name,num,width)  dwarf_regs[name]=make_pair(num,width)
  if (elf_machine == EM_X86_64) {
    DRI ("%rax", 0, DI); DRI ("%eax", 0, SI); DRI ("%ax", 0, HI);
       DRI ("%al", 0, QI); DRI ("%ah", 0, QIh);
    DRI ("%rdx", 1, DI); DRI ("%edx", 1, SI); DRI ("%dx", 1, HI);
       DRI ("%dl", 1, QI); DRI ("%dh", 1, QIh);
    DRI ("%rcx", 2, DI); DRI ("%ecx", 2, SI); DRI ("%cx", 2, HI);
       DRI ("%cl", 2, QI); DRI ("%ch", 2, QIh);
    DRI ("%rbx", 3, DI); DRI ("%ebx", 3, SI); DRI ("%bx", 3, HI);
       DRI ("%bl", 3, QI); DRI ("%bh", 3, QIh);
    DRI ("%rsi", 4, DI); DRI ("%esi", 4, SI); DRI ("%si", 4, HI);
       DRI ("%sil", 4, QI);
    DRI ("%rdi", 5, DI); DRI ("%edi", 5, SI); DRI ("%di", 5, HI);
       DRI ("%dil", 5, QI);
    DRI ("%rbp", 6, DI); DRI ("%ebp", 6, SI); DRI ("%bp", 6, HI);
       DRI ("%bpl", 6, QI);
    DRI ("%rsp", 7, DI); DRI ("%esp", 7, SI); DRI ("%sp", 7, HI);
       DRI ("%spl", 7, QI);
    DRI ("%r8", 8, DI); DRI ("%r8d", 8, SI); DRI ("%r8w", 8, HI);
       DRI ("%r8b", 8, QI);
    DRI ("%r9", 9, DI); DRI ("%r9d", 9, SI); DRI ("%r9w", 9, HI);
       DRI ("%r9b", 9, QI);
    DRI ("%r10", 10, DI); DRI ("%r10d", 10, SI); DRI ("%r10w", 10, HI);
       DRI ("%r10b", 10, QI);
    DRI ("%r11", 11, DI); DRI ("%r11d", 11, SI); DRI ("%r11w", 11, HI);
       DRI ("%r11b", 11, QI);
    DRI ("%r12", 12, DI); DRI ("%r12d", 12, SI); DRI ("%r12w", 12, HI);
       DRI ("%r12b", 12, QI);
    DRI ("%r13", 13, DI); DRI ("%r13d", 13, SI); DRI ("%r13w", 13, HI);
       DRI ("%r13b", 13, QI);
    DRI ("%r14", 14, DI); DRI ("%r14d", 14, SI); DRI ("%r14w", 14, HI);
       DRI ("%r14b", 14, QI);
    DRI ("%r15", 15, DI); DRI ("%r15d", 15, SI); DRI ("%r15w", 15, HI);
       DRI ("%r15b", 15, QI);
    DRI ("%rip", 16, DI); DRI ("%eip", 16, SI); DRI ("%ip", 16, HI);
  } else if (elf_machine == EM_386) {
    DRI ("%eax", 0, SI); DRI ("%ax", 0, HI); DRI ("%al", 0, QI);
       DRI ("%ah", 0, QIh);
    DRI ("%ecx", 1, SI); DRI ("%cx", 1, HI); DRI ("%cl", 1, QI);
       DRI ("%ch", 1, QIh);
    DRI ("%edx", 2, SI); DRI ("%dx", 2, HI); DRI ("%dl", 2, QI);
       DRI ("%dh", 2, QIh);
    DRI ("%ebx", 3, SI); DRI ("%bx", 3, HI); DRI ("%bl", 3, QI);
       DRI ("%bh", 3, QIh);
    DRI ("%esp", 4, SI); DRI ("%sp", 4, HI);
    DRI ("%ebp", 5, SI); DRI ("%bp", 5, HI);
    DRI ("%esi", 6, SI); DRI ("%si", 6, HI); DRI ("%sil", 6, QI);
    DRI ("%edi", 7, SI); DRI ("%di", 7, HI); DRI ("%dil", 7, QI);
  } else if (elf_machine == EM_PPC || elf_machine == EM_PPC64) {
    DRI ("%r0", 0, DI);
    DRI ("%r1", 1, DI);
    DRI ("%r2", 2, DI);
    DRI ("%r3", 3, DI);
    DRI ("%r4", 4, DI);
    DRI ("%r5", 5, DI);
    DRI ("%r6", 6, DI);
    DRI ("%r7", 7, DI);
    DRI ("%r8", 8, DI);
    DRI ("%r9", 9, DI);
    DRI ("%r10", 10, DI);
    DRI ("%r11", 11, DI);
    DRI ("%r12", 12, DI);
    DRI ("%r13", 13, DI);
    DRI ("%r14", 14, DI);
    DRI ("%r15", 15, DI);
    DRI ("%r16", 16, DI);
    DRI ("%r17", 17, DI);
    DRI ("%r18", 18, DI);
    DRI ("%r19", 19, DI);
    DRI ("%r20", 20, DI);
    DRI ("%r21", 21, DI);
    DRI ("%r22", 22, DI);
    DRI ("%r23", 23, DI);
    DRI ("%r24", 24, DI);
    DRI ("%r25", 25, DI);
    DRI ("%r26", 26, DI);
    DRI ("%r27", 27, DI);
    DRI ("%r28", 28, DI);
    DRI ("%r29", 29, DI);
    DRI ("%r30", 30, DI);
    DRI ("%r31", 31, DI);
    // PR11821: unadorned register "names" without -mregnames
    DRI ("0", 0, DI);
    DRI ("1", 1, DI);
    DRI ("2", 2, DI);
    DRI ("3", 3, DI);
    DRI ("4", 4, DI);
    DRI ("5", 5, DI);
    DRI ("6", 6, DI);
    DRI ("7", 7, DI);
    DRI ("8", 8, DI);
    DRI ("9", 9, DI);
    DRI ("10", 10, DI);
    DRI ("11", 11, DI);
    DRI ("12", 12, DI);
    DRI ("13", 13, DI);
    DRI ("14", 14, DI);
    DRI ("15", 15, DI);
    DRI ("16", 16, DI);
    DRI ("17", 17, DI);
    DRI ("18", 18, DI);
    DRI ("19", 19, DI);
    DRI ("20", 20, DI);
    DRI ("21", 21, DI);
    DRI ("22", 22, DI);
    DRI ("23", 23, DI);
    DRI ("24", 24, DI);
    DRI ("25", 25, DI);
    DRI ("26", 26, DI);
    DRI ("27", 27, DI);
    DRI ("28", 28, DI);
    DRI ("29", 29, DI);
    DRI ("30", 30, DI);
    DRI ("31", 31, DI);
  } else if (elf_machine == EM_S390) {
    DRI ("%r0", 0, DI);
    DRI ("%r1", 1, DI);
    DRI ("%r2", 2, DI);
    DRI ("%r3", 3, DI);
    DRI ("%r4", 4, DI);
    DRI ("%r5", 5, DI);
    DRI ("%r6", 6, DI);
    DRI ("%r7", 7, DI);
    DRI ("%r8", 8, DI);
    DRI ("%r9", 9, DI);
    DRI ("%r10", 10, DI);
    DRI ("%r11", 11, DI);
    DRI ("%r12", 12, DI);
    DRI ("%r13", 13, DI);
    DRI ("%r14", 14, DI);
    DRI ("%r15", 15, DI);
  } else if (elf_machine == EM_ARM) {
    DRI ("r0", 0, SI);
    DRI ("r1", 1, SI);
    DRI ("r2", 2, SI);
    DRI ("r3", 3, SI);
    DRI ("r4", 4, SI);
    DRI ("r5", 5, SI);
    DRI ("r6", 6, SI);
    DRI ("r7", 7, SI);
    DRI ("r8", 8, SI);
    DRI ("r9", 9, SI);
    DRI ("r10", 10, SI); DRI ("sl", 10, SI);
    DRI ("fp", 11, SI);
    DRI ("ip", 12, SI);
    DRI ("sp", 13, SI);
    DRI ("lr", 14, SI);
    DRI ("pc", 15, SI);
  } else if (elf_machine == EM_AARCH64) {
    DRI ("x0", 0, DI); DRI ("w0", 0, SI);
    DRI ("x1", 1, DI); DRI ("w1", 1, SI);
    DRI ("x2", 2, DI); DRI ("w2", 2, SI);
    DRI ("x3", 3, DI); DRI ("w3", 3, SI);
    DRI ("x4", 4, DI); DRI ("w4", 4, SI);
    DRI ("x5", 5, DI); DRI ("w5", 5, SI);
    DRI ("x6", 6, DI); DRI ("w6", 6, SI);
    DRI ("x7", 7, DI); DRI ("w7", 7, SI);
    DRI ("x8", 8, DI); DRI ("w8", 8, SI);
    DRI ("x9", 9, DI); DRI ("w9", 9, SI);
    DRI ("x10", 10, DI); DRI ("w10", 10, SI);
    DRI ("x11", 11, DI); DRI ("w11", 11, SI);
    DRI ("x12", 12, DI); DRI ("w12", 12, SI);
    DRI ("x13", 13, DI); DRI ("w13", 13, SI);
    DRI ("x14", 14, DI); DRI ("w14", 14, SI);
    DRI ("x15", 15, DI); DRI ("w15", 15, SI);
    DRI ("x16", 16, DI); DRI ("w16", 16, SI);
    DRI ("x17", 17, DI); DRI ("w17", 17, SI);
    DRI ("x18", 18, DI); DRI ("w18", 18, SI);
    DRI ("x19", 19, DI); DRI ("w19", 19, SI);
    DRI ("x20", 20, DI); DRI ("w20", 20, SI);
    DRI ("x21", 21, DI); DRI ("w21", 21, SI);
    DRI ("x22", 22, DI); DRI ("w22", 22, SI);
    DRI ("x23", 23, DI); DRI ("w23", 23, SI);
    DRI ("x24", 24, DI); DRI ("w24", 24, SI);
    DRI ("x25", 25, DI); DRI ("w25", 25, SI);
    DRI ("x26", 26, DI); DRI ("w26", 26, SI);
    DRI ("x27", 27, DI); DRI ("w27", 27, SI);
    DRI ("x28", 28, DI); DRI ("w28", 28, SI);
    DRI ("x29", 29, DI); DRI ("w29", 29, SI);
    DRI ("x30", 30, DI); DRI ("w30", 30, SI);
    DRI ("sp", 31, DI);
  } else if (arg_count) {
    /* permit this case; just fall back to dwarf */
  }
#undef DRI

  // Build regex pieces out of the known dwarf_regs.  We keep two separate
  // lists: ones with the % prefix (and thus unambigiuous even despite PR11821),
  // and ones with no prefix (and thus only usable in unambiguous contexts).
  map<string,pair<unsigned,int> >::const_iterator ri;
  for (ri = dwarf_regs.begin(); ri != dwarf_regs.end(); ri++)
    {
      string regname = ri->first;
      assert (regname != "");
      regnames += string("|")+regname;
      if (regname[0]=='%')
        percent_regnames += string("|")+regname;
    }

  // clip off leading |
  if (regnames != "")
    regnames = regnames.substr(1);
  if (percent_regnames != "")
    percent_regnames = percent_regnames.substr(1);
}

void
sdt_uprobe_var_expanding_visitor::visit_target_symbol_context (target_symbol* e)
{
  if (e->addressof)
    throw SEMANTIC_ERROR(_("cannot take address of context variable"), e->tok);

  if (e->name == "$$name")
    {
      literal_string *myname = new literal_string (probe_name);
      myname->tok = e->tok;
      provide(myname);
      return;
    }

  else if (e->name == "$$provider")
    {
      literal_string *myname = new literal_string (provider_name);
      myname->tok = e->tok;
      provide(myname);
      return;
    }

  else if (e->name == "$$vars" || e->name == "$$parms")
    {
      e->assert_no_components("sdt", true);

      // Convert $$vars to sprintf of a list of vars which we recursively evaluate

      print_format* pf = print_format::create(e->tok, "sprintf");

      for (unsigned i = 1; i <= arg_count; ++i)
        {
          if (i > 1)
            pf->raw_components += " ";
          target_symbol *tsym = new target_symbol;
          tsym->tok = e->tok;
          tsym->name = "$arg" + lex_cast(i);
          pf->raw_components += tsym->name;
          tsym->components = e->components;

          expression *texp = require<expression> (tsym);
          if (e->check_pretty_print ())
            pf->raw_components += "=%s";
          else
            pf->raw_components += "=%#x";
          pf->args.push_back(texp);
        }

      pf->components = print_format::string_to_components(pf->raw_components);
      provide (pf);
    }
  else
    assert(0); // shouldn't get here
}

unsigned
sdt_uprobe_var_expanding_visitor::get_target_symbol_argno_and_validate (target_symbol *e)
{
  // parsing
  unsigned argno = 0;
  if (startswith(e->name, "$arg"))
    {
      try
        {
          argno = lex_cast<unsigned>(e->name.substr(4));
        }
      catch (const runtime_error& f)
        {
          // non-integral $arg suffix: e.g. $argKKKSDF
          argno = 0;
        }
    }

  // validation
  if (arg_count == 0 || // a sdt.h variant without .probe-stored arg_count
      argno < 1 || argno > arg_count) // a $argN with out-of-range N
    {
      // NB: Either
      // 1) uprobe1_type $argN or $FOO (we don't know the arg_count)
      // 2) uprobe2_type $FOO (no probe args)
      // both of which get resolved later.
      // Throw it now, and it might be resolved by DWARF later.
      need_debug_info = true;
      throw SEMANTIC_ERROR(_("target-symbol requires debuginfo"), e->tok);
    }
  assert (arg_tokens.size() >= argno);
  return argno;
}

long
sdt_uprobe_var_expanding_visitor::parse_out_arg_precision(string& asmarg)
{
  long precision;
  if (asmarg.find('@') != string::npos)
    {
      precision = lex_cast<int>(asmarg.substr(0, asmarg.find('@')));
      asmarg = asmarg.substr(asmarg.find('@')+1);
    }
  else
    {
      // V1/V2 do not have precision field so default to signed long
      // V3 asm does not have precision field so default to unsigned long
      if (probe_type == uprobe3_type)
        precision = sizeof(long); // this is an asm probe
      else
        precision = -sizeof(long);
    }
  return precision;
}

expression*
sdt_uprobe_var_expanding_visitor::try_parse_arg_literal (target_symbol *e,
                                                         const string& asmarg,
                                                         long precision)
{
  expression *argexpr = NULL;

  // Here, we test for a numeric literal.
  // Only accept (signed) decimals throughout. XXX

  // PR11821.  NB: on powerpc, literals are not prefixed with $,
  // so this regex does not match.  But that's OK, since without
  // -mregnames, we can't tell them apart from register numbers
  // anyway.  With -mregnames, we could, if gcc somehow
  // communicated to us the presence of that option, but alas it
  // doesn't.  http://gcc.gnu.org/PR44995.
  vector<string> matches;
  string regexp;

  if (elf_machine == EM_AARCH64) {
    regexp = "^([-]?[0-9][0-9]*)$";
  } else {
    regexp = "^[i\\$#]([-]?[0-9][0-9]*)$";
  }

  if (!regexp_match (asmarg, regexp, matches)) {
      string sn =matches[1];
      int64_t n;

      // We have to pay attention to the size & sign, as gcc sometimes
      // propagates constants that don't quite match, like a negative
      // value to fill an unsigned type.
      // NB: let it throw if something happens
      switch (precision)
        {
        case -1: n = lex_cast<  int8_t>(sn); break;
        case  1: n = lex_cast< uint8_t>(sn); break;
        case -2: n = lex_cast< int16_t>(sn); break;
        case  2: n = lex_cast<uint16_t>(sn); break;
        case -4: n = lex_cast< int32_t>(sn); break;
        case  4: n = lex_cast<uint32_t>(sn); break;
        default:
        case -8: n = lex_cast< int64_t>(sn); break;
        case  8: n = lex_cast<uint64_t>(sn); break;
        }

      literal_number* ln = new literal_number(n);
      ln->tok = e->tok;
      argexpr = ln;
    }

  return argexpr;
}

expression*
sdt_uprobe_var_expanding_visitor::try_parse_arg_register (target_symbol *e,
                                                          const string& asmarg,
                                                          long precision)
{
  expression *argexpr = NULL;

  // test for REGISTER
  // NB: Because PR11821, we must use percent_regnames here.
  string regexp;
  if (elf_machine == EM_PPC || elf_machine == EM_PPC64
     || elf_machine == EM_ARM || elf_machine == EM_AARCH64)
    regexp = "^(" + regnames + ")$";
  else
    regexp = "^(" + percent_regnames + ")$";

  vector<string> matches;
  if (!regexp_match(asmarg, regexp, matches))
    {
      string regname = matches[1];
      map<string,pair<unsigned,int> >::iterator ri = dwarf_regs.find (regname);
      if (ri != dwarf_regs.end()) // known register
        {
          embedded_expr *get_arg1 = new embedded_expr;
          string width_adjust;
          switch (ri->second.second)
            {
            case QI: width_adjust = ") & 0xff)"; break;
            case QIh: width_adjust = ">>8) & 0xff)"; break;
            case HI:
              // preserve 16 bit register signness
              width_adjust = ") & 0xffff)";
              if (precision < 0)
                width_adjust += " << 48 >> 48";
              break;
            case SI:
              // preserve 32 bit register signness
              width_adjust = ") & 0xffffffff)";
              if (precision < 0)
                width_adjust += " << 32 >> 32";
              break;
            default: width_adjust = "))";
            }
          string type = "";
          if (probe_type == uprobe3_type)
            type = (precision < 0
                    ? "(int" : "(uint") + lex_cast(abs(precision) * 8) + "_t)";
          type = type + "((";
          get_arg1->tok = e->tok;
          get_arg1->code = string("/* unprivileged */ /* pure */")
            + string(" ((int64_t)") + type
            + string("u_fetch_register(")
            + lex_cast(dwarf_regs[regname].first) + string("))")
            + width_adjust;
          argexpr = get_arg1;
        }
    }
  return argexpr;
}

static string
precision_to_function(long precision)
{
  switch (precision)
    {
    case 1: case -1:
      return "user_int8";
    case 2:
      return "user_uint16";
    case -2:
      return "user_int16";
    case 4:
      return "user_uint32";
    case -4:
      return "user_int32";
    case 8: case -8:
      return "user_int64";
    default:
      return "user_long";
    }
}

expression*
sdt_uprobe_var_expanding_visitor::try_parse_arg_offset_register (target_symbol *e,
                                                                 const string& asmarg,
                                                                 long precision)
{
  expression *argexpr = NULL;

  // test for OFFSET(REGISTER) where OFFSET is +-N+-N+-N
  // NB: Despite PR11821, we can use regnames here, since the parentheses
  // make things unambiguous. (Note: gdb/stap-probe.c also parses this)
  // On ARM test for [REGISTER, OFFSET]

  string regexp;
  int reg, offset1;
  if (elf_machine == EM_ARM || elf_machine == EM_AARCH64)
    {
      regexp = "^\\[(" + regnames + ")(,[ ]*[#]?([+-]?[0-9]+)([+-][0-9]*)?([+-][0-9]*)?)?\\]$";
      reg = 1;
      offset1 = 3;
    }
  else
    {
      regexp = "^([+-]?[0-9]*)([+-][0-9]*)?([+-][0-9]*)?[(](" + regnames + ")[)]$";
      reg = 4;
      offset1 = 1;
    }

  vector<string> matches;
  if (!regexp_match(asmarg, regexp, matches))
    {
      string regname;
      int64_t disp = 0;
      if (matches[reg].length())
        regname = matches[reg];
      if (dwarf_regs.find (regname) == dwarf_regs.end())
        throw SEMANTIC_ERROR(_F("unrecognized register '%s'", regname.c_str()));

      for (int i=offset1; i <= (offset1 + 2); i++)
        if (matches[i].length())
          // should decode positive/negative hex/decimal
          // NB: let it throw if something happens
          disp += lex_cast<int64_t>(matches[i]);

      // synthesize user_long(%{fetch_register(R)%} + D)
      embedded_expr *get_arg1 = new embedded_expr;
      get_arg1->tok = e->tok;
      get_arg1->code = string("/* unprivileged */ /* pure */")
        + string("u_fetch_register(")
        + lex_cast(dwarf_regs[regname].first) + string(")");
      // XXX: may we ever need to cast that to a narrower type?

      literal_number* inc = new literal_number(disp);
      inc->tok = e->tok;

      binary_expression *be = new binary_expression;
      be->tok = e->tok;
      be->left = get_arg1;
      be->op = "+";
      be->right = inc;

      functioncall *fc = new functioncall;
      fc->function = precision_to_function(precision);
      fc->tok = e->tok;
      fc->args.push_back(be);

      argexpr = fc;
    }

  return argexpr;
}

expression*
sdt_uprobe_var_expanding_visitor::try_parse_arg_effective_addr (target_symbol *e,
                                                                const string& asmarg,
                                                                long precision)
{
  expression *argexpr = NULL;

  // test for OFFSET(BASE_REGISTER,INDEX_REGISTER[,SCALE]) where OFFSET is +-N+-N+-N
  // NB: Despite PR11821, we can use regnames here, since the parentheses
  // make things unambiguous. (Note: gdb/stap-probe.c also parses this)
  string regexp = "^([+-]?[0-9]*)([+-][0-9]*)?([+-][0-9]*)?[(](" + regnames + "),(" +
                                                                   regnames + ")(,[1248])?[)]$";
  vector<string> matches;
  if (!regexp_match(asmarg, regexp, matches))
    {
      string baseregname;
      string indexregname;
      int64_t disp = 0;
      short scale = 1;

      if (matches[6].length())
        // NB: let it throw if we can't cast
        scale = lex_cast<short>(matches[6].substr(1)); // NB: skip the comma!

      if (matches[4].length())
        baseregname = matches[4];
      if (dwarf_regs.find (baseregname) == dwarf_regs.end())
        throw SEMANTIC_ERROR(_F("unrecognized base register '%s'", baseregname.c_str()));

      if (matches[5].length())
        indexregname = matches[5];
      if (dwarf_regs.find (indexregname) == dwarf_regs.end())
        throw SEMANTIC_ERROR(_F("unrecognized index register '%s'", indexregname.c_str()));

      for (int i = 1; i <= 3; i++) // up to three OFFSET terms
        if (matches[i].length())
          // should decode positive/negative hex/decimal
          // NB: let it throw if something happens
          disp += lex_cast<int64_t>(matches[i]);

      // synthesize user_long(%{fetch_register(R1)+fetch_register(R2)*N%} + D)

      embedded_expr *get_arg1 = new embedded_expr;
      string regfn = "u_fetch_register";

      get_arg1->tok = e->tok;
      get_arg1->code = string("/* unprivileged */ /* pure */")
        + regfn + string("(")+lex_cast(dwarf_regs[baseregname].first)+string(")")
        + string("+(")
        + regfn + string("(")+lex_cast(dwarf_regs[indexregname].first)+string(")")
        + string("*")
        + lex_cast(scale)
        + string(")");

      // NB: could plop this +DISPLACEMENT bit into the embedded-c expression too
      literal_number* inc = new literal_number(disp);
      inc->tok = e->tok;

      binary_expression *be = new binary_expression;
      be->tok = e->tok;
      be->left = get_arg1;
      be->op = "+";
      be->right = inc;

      functioncall *fc = new functioncall;
      fc->function = precision_to_function(precision);
      fc->tok = e->tok;
      fc->args.push_back(be);

      argexpr = fc;
    }

  return argexpr;
}

expression*
sdt_uprobe_var_expanding_visitor::try_parse_arg_varname (target_symbol *e,
                                                         const string& asmarg,
                                                         long precision)
{
  static unsigned tick = 0;
  expression *argexpr = NULL;

  // test for [OFF+]VARNAME[+OFF][(REGISTER)], where VARNAME is a variable
  // name. NB: Despite PR11821, we can use regnames here, since the parentheses
  // make things unambiguous.
  string regex = "^(([0-9]+)[+])?([a-zA-Z_][a-zA-Z0-9_]*)([+][0-9]+)?([(]("
                 + regnames + ")[)])?$";
  vector<string> matches;
  if (!regexp_match(asmarg, regex, matches))
    {
      assert(matches.size() >= 4);
      string varname = matches[3];

      // OFF can be before VARNAME (put in matches[2]) or after (put in
      // matches[4]) (or both?). Seems like in most cases it comes after,
      // unless the code was compiled with -fPIC.
      int64_t offset = 0;
      if (!matches[2].empty())
        offset += lex_cast<int64_t>(matches[2]);
      if (matches.size() >= 5 && !matches[4].empty())
        offset += lex_cast<int64_t>(matches[4]);

      string regname;
      if (matches.size() >= 7)
        regname = matches[6];

      // If it's just VARNAME, then proceed. If it's VARNAME(REGISTER), then
      // only proceed if it's RIP-relative addressing on x86_64.
      if (regname.empty() || (regname == "%rip" && elf_machine == EM_X86_64))
        {
          dw.mod_info->get_symtab();
          if (dw.mod_info->symtab_status != info_present)
            throw SEMANTIC_ERROR(_("can't retrieve symbol table"));

          assert(dw.mod_info->sym_table);
          map<string, Dwarf_Addr>& globals = dw.mod_info->sym_table->globals;
          map<string, Dwarf_Addr>& locals = dw.mod_info->sym_table->locals;
          Dwarf_Addr addr = 0;

          // check symtab locals then globals
          if (locals.count(varname))
            addr = locals[varname];
          if (globals.count(varname))
            addr = globals[varname];

          if (addr)
            {
              // add whatever offset is in the operand
              addr += offset;

              // adjust for dw bias because relocate_address() expects a
              // libdw address and this addr is from the symtab
              dw.get_module_dwarf(false, false);
              addr -= dw.module_bias;

              string reloc_section;
              Dwarf_Addr reloc_addr = dw.relocate_address(addr, reloc_section);

              // OK, we have an address for the variable. Let's create a
              // function that will just relocate it at runtime, and then
              // call user_[u]int*() on the address it returns.

              functioncall *user_int_call = new functioncall;
              user_int_call->function = precision_to_function(precision);
              user_int_call->tok = e->tok;

              functiondecl *get_addr_decl = new functiondecl;
              get_addr_decl->tok = e->tok;
              get_addr_decl->synthetic = true;
              get_addr_decl->name = "_sdt_arg_get_addr_" + lex_cast(tick++);
              get_addr_decl->type = pe_long;

              // build _stp_umodule_relocate(module, addr, current)
              stringstream ss;
              ss << " /* unprivileged */ /* pure */ /* pragma:vma */" << endl;
              ss << "STAP_RETURN(_stp_umodule_relocate(";
                ss << "\"" << path_remove_sysroot(session, process_name) << "\", ";
                ss << "0x" << hex << reloc_addr << dec << ", ";
                ss << "current";
              ss << "));" << endl;

              embeddedcode *ec = new embeddedcode;
              ec->tok = e->tok;
              ec->code = ss.str();
              get_addr_decl->body = ec;
              get_addr_decl->join(session);

              functioncall *get_addr_call = new functioncall;
              get_addr_call->tok = e->tok;
              get_addr_call->function = get_addr_decl->name;
              user_int_call->args.push_back(get_addr_call);

              argexpr = user_int_call;
            }
        }
    }

  return argexpr;
}

void
sdt_uprobe_var_expanding_visitor::visit_target_symbol_arg (target_symbol *e)
{
  try
    {
      unsigned argno = get_target_symbol_argno_and_validate(e); // the N in $argN
      string asmarg = arg_tokens[argno-1];   // $arg1 => arg_tokens[0]

      // Now we try to parse this thing, which is an assembler operand
      // expression.  If we can't, we warn, back down to need_debug_info
      // and hope for the best.  Here is the syntax for a few architectures.
      // Note that the power iN syntax is only for V3 sdt.h; gcc emits the i.
      //
      //        literal reg reg      reg+     base+index*size+ VAR VAR+off RIP-relative
      //                    indirect offset   offset                       VAR+off
      // x86    $N      %rR (%rR)    N(%rR)   O(%bR,%iR,S)     var var+off var+off(%rip)
      // x86_64 $N      %rR (%rR)    N(%rR)   O(%bR,%iR,S)     var var+off var+off(%rip)
      // power  iN      R   (R)      N(R)
      // ia64   N       rR  [r16]
      // s390   N       %rR 0(rR)    N(r15)
      // arm    #N      rR  [rR]     [rR, #N]
      // arm64  N       rR  [rR]     [rR, N]

      expression* argexpr = 0; // filled in in case of successful parse

      // Parse (and remove from asmarg) the leading length
      long precision = parse_out_arg_precision(asmarg);

      try
        {
          if ((argexpr = try_parse_arg_literal(e, asmarg, precision)) != NULL)
            goto matched;

          // all other matches require registers
          if (regnames == "")
            throw SEMANTIC_ERROR("no registers to use for parsing");

          if ((argexpr = try_parse_arg_register(e, asmarg, precision)) != NULL)
            goto matched;
          if ((argexpr = try_parse_arg_offset_register(e, asmarg, precision)) != NULL)
            goto matched;
          if ((argexpr = try_parse_arg_effective_addr(e, asmarg, precision)) != NULL)
            goto matched;
          if ((argexpr = try_parse_arg_varname(e, asmarg, precision)) != NULL)
            goto matched;
        }
      catch (const semantic_error& err)
        {
          e->chain(err);
        }

      // The asmarg operand was not recognized.  Back down to dwarf.
      if (! session.suppress_warnings)
        {
          if (probe_type == UPROBE3_TYPE)
            session.print_warning (_F("Can't parse SDT_V3 operand '%s' "
                                      "[man error::sdt]", asmarg.c_str()),
                                   e->tok);
          else // must be *PROBE2; others don't get asm operands
            session.print_warning (_F("Downgrading SDT_V2 probe argument to "
                                      "dwarf, can't parse '%s' [man error::sdt]",
                                      asmarg.c_str()),
                                   e->tok);
        }

      need_debug_info = true;
      throw SEMANTIC_ERROR(_("SDT asm not understood, requires debuginfo "
                             "[man error::sdt]"), e->tok);

      /* NOTREACHED */

    matched:
      assert (argexpr != 0);

      if (session.verbose > 2)
        //TRANSLATORS: We're mapping the operand to a new expression*.
        clog << _F("mapped asm operand %s to ", asmarg.c_str()) << *argexpr << endl;

      if (e->components.empty()) // We have a scalar
        {
          if (e->addressof)
            throw SEMANTIC_ERROR(_("cannot take address of sdt variable"), e->tok);
          provide (argexpr);
        }
      else  // $var->foo
        {
          cast_op *cast = new cast_op;
          cast->name = "@cast";
          cast->tok = e->tok;
          cast->operand = argexpr;
          cast->components = e->components;
          cast->type_name = probe_name + "_arg" + lex_cast(argno);
          cast->module = process_name;
          cast->visit(this);
        }
    }
  catch (const semantic_error &er)
    {
      e->chain (er);
      provide (e);
    }
}


void
sdt_uprobe_var_expanding_visitor::visit_target_symbol (target_symbol* e)
{
  try
    {
      assert(e->name.size() > 0
             && (e->name[0] == '$' || e->name == "@var"));

      if (e->name == "$$name" || e->name == "$$provider" || e->name == "$$parms" || e->name == "$$vars")
        visit_target_symbol_context (e);
      else
        visit_target_symbol_arg (e);
    }
  catch (const semantic_error &er)
    {
      e->chain (er);
      provide (e);
    }
}


void
sdt_uprobe_var_expanding_visitor::visit_atvar_op (atvar_op* e)
{
  need_debug_info = true;

  // Fill in our current module context if needed
  if (e->module.empty())
    e->module = process_name;

  var_expanding_visitor::visit_atvar_op(e);
}


void
sdt_uprobe_var_expanding_visitor::visit_cast_op (cast_op* e)
{
  // Fill in our current module context if needed
  if (e->module.empty())
    e->module = process_name;

  var_expanding_visitor::visit_cast_op(e);
}


void
plt_expanding_visitor::visit_target_symbol (target_symbol *e)
{
  try
    {
      if (e->name == "$$name")
	{
	  literal_string *myname = new literal_string (entry);
	  myname->tok = e->tok;
	  provide(myname);
	  return;
	}

      // variable not found -> throw a semantic error
      // (only to be caught right away, but this may be more complex later...)
      string alternatives = "$$name";
      throw SEMANTIC_ERROR(_F("unable to find plt variable '%s' (alternatives: %s)",
                              e->name.c_str(), alternatives.c_str()), e->tok);
    }
  catch (const semantic_error &er)
    {
      e->chain (er);
      provide (e);
    }
}


struct sdt_query : public base_query
{
  sdt_query(probe * base_probe, probe_point * base_loc,
            dwflpp & dw, literal_map_t const & params,
            vector<derived_probe *> & results, const string user_lib);

  void query_library (const char *data);
  set<string> visited_libraries;
  bool resolved_library;

  void query_plt (const char *entry, size_t addr) {}
  void handle_query_module();

private:
  stap_sdt_probe_type probe_type;
  enum { probe_section=0, note_section=1, unknown_section=-1 } probe_loc;
  probe * base_probe;
  probe_point * base_loc;
  literal_map_t const & params;
  vector<derived_probe *> & results;
  string pp_mark;
  string pp_provider;
  string user_lib;

  set<string> probes_handled;

  Elf_Data *pdata;
  size_t probe_scn_offset;
  size_t probe_scn_addr;
  uint64_t arg_count;
  GElf_Addr base;
  GElf_Addr pc;
  string arg_string;
  string probe_name;
  string provider_name;
  GElf_Addr semaphore_load_offset;
  Dwarf_Addr semaphore;

  bool init_probe_scn();
  bool get_next_probe();
  void iterate_over_probe_entries();
  void handle_probe_entry();

  static void setup_note_probe_entry_callback (sdt_query *me,
                                               const string& scn_name,
                                               const string& note_name,
                                               int type,
                                               const char *data,
                                               size_t len);
  void setup_note_probe_entry (const string& scn_name,
                               const string& note_name, int type,
                               const char *data, size_t len);

  void convert_probe(probe *base);
  void record_semaphore(vector<derived_probe *> & results, unsigned start);
  probe* convert_location();
  bool have_uprobe() {return probe_type == uprobe1_type || probe_type == uprobe2_type || probe_type == uprobe3_type;}
  bool have_debuginfo_uprobe(bool need_debug_info)
  {return probe_type == uprobe1_type
      || ((probe_type == uprobe2_type || probe_type == uprobe3_type)
	  && need_debug_info);}
  bool have_debuginfoless_uprobe() {return probe_type == uprobe2_type || probe_type == uprobe3_type;}
};


sdt_query::sdt_query(probe * base_probe, probe_point * base_loc,
                     dwflpp & dw, literal_map_t const & params,
                     vector<derived_probe *> & results, const string user_lib):
  base_query(dw, params), resolved_library(false),
  probe_type(unknown_probe_type), probe_loc(unknown_section),
  base_probe(base_probe), base_loc(base_loc), params(params), results(results),
  user_lib(user_lib), probe_scn_offset(0), probe_scn_addr(0), arg_count(0),
  base(0), pc(0), semaphore_load_offset(0), semaphore(0)
{
  assert(get_string_param(params, TOK_MARK, pp_mark));
  get_string_param(params, TOK_PROVIDER, pp_provider); // pp_provider == "" -> unspecified

  // PR10245: permit usage of dtrace-y "-" separator in marker name;
  // map it to double-underscores.
  size_t pos = 0;
  while (1) // there may be more than one
    {
      size_t i = pp_mark.find("-", pos);
      if (i == string::npos) break;
      pp_mark.replace (i, 1, "__");
      pos = i+1; // resume searching after the inserted __
    }

  // XXX: same for pp_provider?
}


void
sdt_query::handle_probe_entry()
{
  if (! have_uprobe()
      && !probes_handled.insert(probe_name).second)
    return;

  if (sess.verbose > 3)
    {
      //TRANSLATORS: Describing what probe type (kprobe or uprobe) the probe
      //TRANSLATORS: is matched to.
      clog << _F("matched probe_name %s probe type ", probe_name.c_str());
      switch (probe_type)
	{
	case uprobe1_type:
	  clog << "uprobe1 at 0x" << hex << pc << dec << endl;
	  break;
	case uprobe2_type:
	  clog << "uprobe2 at 0x" << hex << pc << dec << endl;
	  break;
	case uprobe3_type:
	  clog << "uprobe3 at 0x" << hex << pc << dec << endl;
	  break;
	default:
	  clog << "unknown!" << endl;
	  break;
	}
    }

  // Extend the derivation chain
  probe *new_base = convert_location();
  probe_point *new_location = new_base->locations[0];

  bool need_debug_info = false;

  // We could get the Elf* from either dwarf_getelf(dwfl_module_getdwarf(...))
  // or dwfl_module_getelf(...).  We only need it for the machine type, which
  // should be the same.  The bias is used for relocating debuginfoless probes,
  // though, so that must come from the possibly-prelinked ELF file, not DWARF.
  Dwarf_Addr bias;
  Elf* elf = dwfl_module_getelf (dw.mod_info->mod, &bias);

  /* Figure out the architecture of this particular ELF file.  The
     dwarfless register-name mappings depend on it. */
  GElf_Ehdr ehdr_mem;
  GElf_Ehdr* em = gelf_getehdr (elf, &ehdr_mem);
  if (em == 0) { DWFL_ASSERT ("dwfl_getehdr", dwfl_errno()); }
  assert(em);
  int elf_machine = em->e_machine;
  sdt_uprobe_var_expanding_visitor svv (sess, dw, elf_machine, module_val,
                                        provider_name, probe_name, probe_type,
                                        arg_string, arg_count);
  svv.replace (new_base->body);
  need_debug_info = svv.need_debug_info;

  // XXX: why not derive_probes() in the uprobes case too?
  literal_map_t params;
  for (unsigned i = 0; i < new_location->components.size(); ++i)
   {
      probe_point::component *c = new_location->components[i];
      params[c->functor] = c->arg;
   }

  unsigned prior_results_size = results.size();
  dwarf_query q(new_base, new_location, dw, params, results, "", "");
  q.has_mark = true; // enables mid-statement probing

  // V1 probes always need dwarf info
  // V2+ probes need dwarf info in case of a variable reference
  if (have_debuginfo_uprobe(need_debug_info))
    dw.iterate_over_modules<base_query>(&query_module, &q);

  // For V2+ probes, if variable references weren't used or failed (PR14369),
  // then try with the more direct approach.  Unresolved $vars might still
  // cause their own error, but this gives them a chance to be optimized out.
  if (have_debuginfoless_uprobe() && results.size() == prior_results_size)
    {
      string section;
      Dwarf_Addr reloc_addr = q.statement_num_val + bias;
      if (dwfl_module_relocations (q.dw.mod_info->mod) > 0)
        {
	  dwfl_module_relocate_address (q.dw.mod_info->mod, &reloc_addr);
	  section = ".dynamic";
        }
      else
	section = ".absolute";

      uprobe_derived_probe* p =
	new uprobe_derived_probe ("", "", 0,
				  path_remove_sysroot(sess,q.module_val),
				  section,
				  q.statement_num_val, reloc_addr, q, 0);
      p->saveargs (arg_count);
      results.push_back (p);
    }
  sess.unwindsym_modules.insert (dw.module_name);
  record_semaphore(results, prior_results_size);
}


void
sdt_query::handle_query_module()
{
  if (!init_probe_scn())
    return;

  if (sess.verbose > 3)
    clog << "TOK_MARK: " << pp_mark << " TOK_PROVIDER: " << pp_provider << endl;

  if (probe_loc == note_section)
    {
      GElf_Shdr shdr_mem;
      GElf_Shdr *shdr = dw.get_section (".stapsdt.base", &shdr_mem);

      // The 'base' lets us adjust the hardcoded addresses in notes for prelink
      // effects.  The 'semaphore_load_offset' accounts for the difference in
      // load addresses between text and data, so the semaphore can be
      // converted to a file offset if needed.
      if (shdr)
	{
	  base = shdr->sh_addr;
	  GElf_Addr base_offset = shdr->sh_offset;
	  shdr = dw.get_section (".probes", &shdr_mem);
	  if (shdr)
	    semaphore_load_offset =
	      (shdr->sh_addr - shdr->sh_offset) - (base - base_offset);
	}
      else
	base = semaphore_load_offset = 0;

      dw.iterate_over_notes (this, &sdt_query::setup_note_probe_entry_callback);
    }
  else if (probe_loc == probe_section)
    iterate_over_probe_entries ();
}


bool
sdt_query::init_probe_scn()
{
  Elf* elf;
  GElf_Shdr shdr_mem;

  GElf_Shdr *shdr = dw.get_section (".note.stapsdt", &shdr_mem);
  if (shdr)
    {
      probe_loc = note_section;
      return true;
    }

  shdr = dw.get_section (".probes", &shdr_mem, &elf);
  if (shdr)
    {
      pdata = elf_getdata_rawchunk (elf, shdr->sh_offset, shdr->sh_size, ELF_T_BYTE);
      probe_scn_offset = 0;
      probe_scn_addr = shdr->sh_addr;
      assert (pdata != NULL);
      if (sess.verbose > 4)
        clog << "got .probes elf scn_addr@0x" << probe_scn_addr << ", size: "
             << pdata->d_size << endl;
      probe_loc = probe_section;
      return true;
    }
  else
    return false;
}

void
sdt_query::setup_note_probe_entry_callback (sdt_query *me,
                                            const string& scn_name,
                                            const string& note_name, int type,
                                            const char *data, size_t len)
{
  me->setup_note_probe_entry (scn_name, note_name, type, data, len);
}


void
sdt_query::setup_note_probe_entry (const string& scn_name,
                                   const string& note_name, int type,
                                   const char *data, size_t len)
{
  if (scn_name.compare(".note.stapsdt"))
    return;
#define _SDT_NOTE_NAME "stapsdt"
  if (note_name.compare(_SDT_NOTE_NAME))
    return;
#define _SDT_NOTE_TYPE 3
  if (type != _SDT_NOTE_TYPE)
    return;

  // we found a probe entry
  union
  {
    Elf64_Addr a64[3];
    Elf32_Addr a32[3];
  } buf;
  Dwarf_Addr bias;
  Elf* elf = (dwfl_module_getelf (dw.mod_info->mod, &bias));
  Elf_Data dst =
    {
      &buf, ELF_T_ADDR, EV_CURRENT,
      gelf_fsize (elf, ELF_T_ADDR, 3, EV_CURRENT), 0, 0
    };
  assert (dst.d_size <= sizeof buf);

  if (len < dst.d_size + 3)
    return;

  Elf_Data src =
    {
      (void *) data, ELF_T_ADDR, EV_CURRENT,
      dst.d_size, 0, 0
    };

  if (gelf_xlatetom (elf, &dst, &src,
		      elf_getident (elf, NULL)[EI_DATA]) == NULL)
    printf ("gelf_xlatetom: %s", elf_errmsg (-1));

  probe_type = uprobe3_type;
  const char * provider = data + dst.d_size;

  const char *name = (const char*)memchr (provider, '\0', data + len - provider);
  if(name++ == NULL)
    return;

  const char *args = (const char*)memchr (name, '\0', data + len - name);
  if (args++ == NULL || memchr (args, '\0', data + len - name) != data + len - 1)
    return;

  provider_name = provider;
  probe_name = name;
  arg_string = args;

  dw.mod_info->marks.insert(make_pair(provider, name));

  // Did we find a matching probe?
  if (! (dw.function_name_matches_pattern (probe_name, pp_mark)
	 && ((pp_provider == "")
	     || dw.function_name_matches_pattern (provider_name, pp_provider))))
    return;

  // PR13934: Assembly probes are not forced to use the N@OP form.
  // If we have '@' then great, else count based on space-delimiters.
  arg_count = count(arg_string.begin(), arg_string.end(), '@');
  if (!arg_count && !arg_string.empty())
    arg_count = 1 + count(arg_string.begin(), arg_string.end(), ' ');

  GElf_Addr base_ref;
  if (gelf_getclass (elf) == ELFCLASS32)
    {
      pc = buf.a32[0];
      base_ref = buf.a32[1];
      semaphore = buf.a32[2];
    }
  else
    {
      pc = buf.a64[0];
      base_ref = buf.a64[1];
      semaphore = buf.a64[2];
    }

  semaphore += base - base_ref;
  pc += base - base_ref;

  // The semaphore also needs the ELF bias added now, so
  // record_semaphore can properly relocate it later.
  semaphore += bias;

  if (sess.verbose > 4)
    clog << _F(" saw .note.stapsdt %s%s ", probe_name.c_str(), (provider_name != "" ? _(" (provider ")+provider_name+") " : "").c_str()) << "@0x" << hex << pc << dec << endl;

  handle_probe_entry();
}


void
sdt_query::iterate_over_probe_entries()
{
  // probes are in the .probe section
  while (probe_scn_offset < pdata->d_size)
    {
      stap_sdt_probe_entry_v1 *pbe_v1 = (stap_sdt_probe_entry_v1 *) ((char*)pdata->d_buf + probe_scn_offset);
      stap_sdt_probe_entry_v2 *pbe_v2 = (stap_sdt_probe_entry_v2 *) ((char*)pdata->d_buf + probe_scn_offset);
      probe_type = (stap_sdt_probe_type)(pbe_v1->type_a);
      if (! have_uprobe())
	{
	  // Unless this is a mangled .probes section, this happens
	  // because the name of the probe comes first, followed by
	  // the sentinel.
	  if (sess.verbose > 5)
            clog << _F("got unknown probe_type : 0x%x", probe_type) << endl;
	  probe_scn_offset += sizeof(__uint32_t);
	  continue;
	}
      if ((long)pbe_v1 % sizeof(__uint64_t)) // we have stap_sdt_probe_entry_v1.type_b
	{
	  pbe_v1 = (stap_sdt_probe_entry_v1*)((char*)pbe_v1 - sizeof(__uint32_t));
	  if (pbe_v1->type_b != uprobe1_type)
	    continue;
	}

      if (probe_type == uprobe1_type)
	{
	  if (pbe_v1->name == 0) // No name possibly means we have a .so with a relocation
	    return;
	  semaphore = 0;
	  probe_name = (char*)((char*)pdata->d_buf + pbe_v1->name - (char*)probe_scn_addr);
          provider_name = ""; // unknown
	  pc = pbe_v1->arg;
	  arg_count = 0;
	  probe_scn_offset += sizeof (stap_sdt_probe_entry_v1);
	}
      else if (probe_type == uprobe2_type)
	{
	  if (pbe_v2->name == 0) // No name possibly means we have a .so with a relocation
	    return;
	  semaphore = pbe_v2->semaphore;
	  probe_name = (char*)((char*)pdata->d_buf + pbe_v2->name - (char*)probe_scn_addr);
	  provider_name = (char*)((char*)pdata->d_buf + pbe_v2->provider - (char*)probe_scn_addr);
	  arg_count = pbe_v2->arg_count;
	  pc = pbe_v2->pc;
	  if (pbe_v2->arg_string)
	    arg_string = (char*)((char*)pdata->d_buf + pbe_v2->arg_string - (char*)probe_scn_addr);
	  // skip over pbe_v2, probe_name text and provider text
	  probe_scn_offset = ((long)(pbe_v2->name) - (long)(probe_scn_addr)) + probe_name.length();
	  probe_scn_offset += sizeof (__uint32_t) - probe_scn_offset % sizeof (__uint32_t);
	}
      if (sess.verbose > 4)
	clog << _("saw .probes ") << probe_name << (provider_name != "" ? _(" (provider ")+provider_name+") " : "")
	     << "@0x" << hex << pc << dec << endl;

      dw.mod_info->marks.insert(make_pair(provider_name, probe_name));

      if (dw.function_name_matches_pattern (probe_name, pp_mark)
          && ((pp_provider == "") || dw.function_name_matches_pattern (provider_name, pp_provider)))
	handle_probe_entry ();
    }
}


void
sdt_query::record_semaphore (vector<derived_probe *> & results, unsigned start)
{
  for (unsigned i=0; i<2; i++) {
    // prefer with-provider symbol; look without provider prefix for backward compatibility only
    string semaphore = (i==0 ? (provider_name+"_") : "") + probe_name + "_semaphore";
    // XXX: multiple addresses?
    if (sess.verbose > 2)
      clog << _F("looking for semaphore symbol %s ", semaphore.c_str());

    Dwarf_Addr addr;
    if (this->semaphore)
      addr = this->semaphore;
    else
      addr  = lookup_symbol_address(dw.module, semaphore.c_str());
    if (addr)
      {
        if (dwfl_module_relocations (dw.module) > 0)
          dwfl_module_relocate_address (dw.module, &addr);
        // XXX: relocation basis?

        // Dyninst needs the *file*-based offset for semaphores,
        // so subtract the difference in load addresses between .text and .probes
        if (dw.sess.runtime_usermode_p())
          addr -= semaphore_load_offset;

        for (unsigned i = start; i < results.size(); ++i)
          results[i]->sdt_semaphore_addr = addr;
        if (sess.verbose > 2)
          clog << _(", found at 0x") << hex << addr << dec << endl;
        return;
      }
    else
      if (sess.verbose > 2)
        clog << _(", not found") << endl;
  }
}


void
sdt_query::convert_probe (probe *base)
{
  block *b = new block;
  b->tok = base->body->tok;

  // Generate: if (arg1 != mark("label")) next;
  functioncall *fc = new functioncall;
  fc->function = "ulong_arg";
  fc->tok = b->tok;
  literal_number* num = new literal_number(1);
  num->tok = b->tok;
  fc->args.push_back(num);

  functioncall *fcus = new functioncall;
  fcus->function = "user_string";
  fcus->type = pe_string;
  fcus->tok = b->tok;
  fcus->args.push_back(fc);

  if_statement *is = new if_statement;
  is->thenblock = new next_statement;
  is->elseblock = NULL;
  is->tok = b->tok;
  is->thenblock->tok = b->tok;
  comparison *be = new comparison;
  be->op = "!=";
  be->tok = b->tok;
  be->left = fcus;
  be->right = new literal_string(probe_name);
  be->right->tok = b->tok;
  is->condition = be;
  b->statements.push_back(is);

  // Now replace the body
  b->statements.push_back(base->body);
  base->body = b;
}


probe*
sdt_query::convert_location ()
{
  string module = dw.module_name;
  if (has_process)
    module = path_remove_sysroot(sess, module);

  probe_point* specific_loc = new probe_point(*base_loc);
  specific_loc->well_formed = true;

  vector<probe_point::component*> derived_comps;

  vector<probe_point::component*>::iterator it;
  for (it = specific_loc->components.begin();
       it != specific_loc->components.end(); ++it)
    if ((*it)->functor == TOK_PROCESS)
      {
        // replace the possibly incomplete path to process
        *it = new probe_point::component(TOK_PROCESS,
                new literal_string(has_library ? path : module));

        // copy the process name
        derived_comps.push_back(*it);
      }
    else if ((*it)->functor == TOK_LIBRARY)
      {
        // copy the library name for process probes
        derived_comps.push_back(*it);
      }
    else if ((*it)->functor == TOK_PROVIDER)
      {
        // replace the possibly wildcarded arg with the specific provider name
        *it = new probe_point::component(TOK_PROVIDER,
                                         new literal_string(provider_name));
      }
    else if ((*it)->functor == TOK_MARK)
      {
        // replace the possibly wildcarded arg with the specific marker name
        *it = new probe_point::component(TOK_MARK,
                                         new literal_string(probe_name));

	if (sess.verbose > 3)
	  switch (probe_type)
	    {
	    case uprobe1_type:
              clog << _("probe_type == uprobe1, use statement addr: 0x")
		   << hex << pc << dec << endl;
	      break;
	    case uprobe2_type:
              clog << _("probe_type == uprobe2, use statement addr: 0x")
		   << hex << pc << dec << endl;
            break;
	    case uprobe3_type:
              clog << _("probe_type == uprobe3, use statement addr: 0x")
		   << hex << pc << dec << endl;
	      break;
	    default:
              clog << _F("probe_type == use_uprobe_no_dwarf, use label name: _stapprobe1_%s",
                         pp_mark.c_str()) << endl;
          }

        switch (probe_type)
          {
          case uprobe1_type:
          case uprobe2_type:
          case uprobe3_type:
            // process("executable").statement(probe_arg)
            derived_comps.push_back
              (new probe_point::component(TOK_STATEMENT,
                                          new literal_number(pc, true)));
            break;

          default: // deprecated
            // process("executable").function("*").label("_stapprobe1_MARK_NAME")
            derived_comps.push_back
              (new probe_point::component(TOK_FUNCTION,
                                          new literal_string("*")));
            derived_comps.push_back
              (new probe_point::component(TOK_LABEL,
                                          new literal_string("_stapprobe1_" + pp_mark)));
            break;
          }
      }

  probe_point* derived_loc = new probe_point(*specific_loc);
  derived_loc->components = derived_comps;
  return new probe (new probe (base_probe, specific_loc), derived_loc);
}


void
sdt_query::query_library (const char *library)
{
  visited_libraries.insert(library);
  if (query_one_library (library, dw, user_lib, base_probe, base_loc, results))
    resolved_library = true;
}

string
suggest_marks(systemtap_session& sess,
              const set<string>& modules,
              const string& mark,
              const string& provider)
{
  if (mark.empty() || modules.empty() || sess.module_cache == NULL)
    return "";

  // PR18577: There isn't any point in generating a suggestion list if
  // we're not going to display it.
  if ((sess.dump_mode == systemtap_session::dump_matched_probes
       || sess.dump_mode == systemtap_session::dump_matched_probes_vars)
      && sess.verbose < 2)
    return "";

  set<string> marks;
  const map<string, module_info*> &cache = sess.module_cache->cache;
  bool dash_suggestions = (mark.find("-") != string::npos);

  for (set<string>::iterator itmod = modules.begin();
       itmod != modules.end(); ++itmod)
    {
      map<string, module_info*>::const_iterator itcache;
      if ((itcache = cache.find(*itmod)) != cache.end())
        {
          set<pair<string,string> >::const_iterator itmarks;
          for (itmarks = itcache->second->marks.begin();
               itmarks != itcache->second->marks.end(); ++itmarks)
            {
              if (provider.empty()
                  // simulating dw.function_name_matches_pattern()
                  || (fnmatch(provider.c_str(), itmarks->first.c_str(), 0) == 0))
                {
                  string marksug = itmarks->second;
                  if (dash_suggestions)
                    {
                      size_t pos = 0;
                      while (1) // there may be more than one
                        {
                          size_t i = marksug.find("__", pos);
                          if (i == string::npos) break;
                          marksug.replace (i, 2, "-");
                          pos = i+1; // resume searching after the inserted -
                        }
                    }
                  marks.insert(marksug);
                }
            }
        }
    }

  if (sess.verbose > 2)
    {
      clog << "suggesting " << marks.size() << " marks "
           << "from modules:" << endl;
      for (set<string>::iterator itmod = modules.begin();
          itmod != modules.end(); ++itmod)
        clog << *itmod << endl;
    }

  if (marks.empty())
    return "";

  return levenshtein_suggest(mark, marks, 5); // print top 5 marks only
}

string
suggest_plt_functions(systemtap_session& sess,
                      const set<string>& modules,
                      const string& func)
{
  if (func.empty() || modules.empty() || sess.module_cache == NULL)
    return "";

  // PR18577: There isn't any point in generating a suggestion list if
  // we're not going to display it.
  if ((sess.dump_mode == systemtap_session::dump_matched_probes
       || sess.dump_mode == systemtap_session::dump_matched_probes_vars)
      && sess.verbose < 2)
    return "";

  set<string> funcs;
  const map<string, module_info*> &cache = sess.module_cache->cache;

  for (set<string>::iterator itmod = modules.begin();
       itmod != modules.end(); ++itmod)
    {
      map<string, module_info*>::const_iterator itcache;
      if ((itcache = cache.find(*itmod)) != cache.end())
        funcs.insert(itcache->second->plt_funcs.begin(),
                     itcache->second->plt_funcs.end());
    }

  if (sess.verbose > 2)
    {
      clog << "suggesting " << funcs.size() << " plt functions "
           << "from modules:" << endl;
      for (set<string>::iterator itmod = modules.begin();
          itmod != modules.end(); ++itmod)
        clog << *itmod << endl;
    }

  if (funcs.empty())
    return "";

  return levenshtein_suggest(func, funcs, 5); // print top 5 funcs only
}

string
suggest_dwarf_functions(systemtap_session& sess,
                        const set<string>& modules,
                        string func)
{
  // Trim any @ component
  size_t pos = func.find('@');
  if (pos != string::npos)
    func.erase(pos);

  if (func.empty() || modules.empty() || sess.module_cache == NULL)
    return "";

  // PR18577: There isn't any point in generating a suggestion list if
  // we're not going to display it.
  if ((sess.dump_mode == systemtap_session::dump_matched_probes
       || sess.dump_mode == systemtap_session::dump_matched_probes_vars)
      && sess.verbose < 2)
    return "";

  // We must first aggregate all the functions from the cache
  set<string> funcs;
  const map<string, module_info*> &cache = sess.module_cache->cache;

  for (set<string>::iterator itmod = modules.begin();
      itmod != modules.end(); ++itmod)
    {
      module_info *module;

      // retrieve module_info from cache
      map<string, module_info*>::const_iterator itcache;
      if ((itcache = cache.find(*itmod)) != cache.end())
        module = itcache->second;
      else // module not found
        continue;

      // add inlines
      funcs.insert(module->inlined_funcs.begin(),
                   module->inlined_funcs.end());

      // add all function symbols in cache
      if (module->symtab_status != info_present || module->sym_table == NULL)
        continue;
      multimap<string, func_info*>& modfuncs = module->sym_table->map_by_name;
      for (multimap<string, func_info*>::const_iterator itfuncs = modfuncs.begin();
           itfuncs != modfuncs.end(); ++itfuncs)
        funcs.insert(itfuncs->first);
    }

  if (sess.verbose > 2)
    {
      clog << "suggesting " << funcs.size() << " dwarf functions "
           << "from modules:" << endl;
      for (set<string>::iterator itmod = modules.begin();
          itmod != modules.end(); ++itmod)
        clog << *itmod << endl;
    }

  if (funcs.empty())
    return "";

  return levenshtein_suggest(func, funcs, 5); // print top 5 funcs only
}


// Use a glob pattern to find executables or shared libraries
static set<string>
glob_executable(const string& pattern)
{
  glob_t the_blob;
  set<string> globs;

  int rc = glob (pattern.c_str(), 0, NULL, & the_blob);
  if (rc)
    throw SEMANTIC_ERROR (_F("glob %s error (%d)", pattern.c_str(), rc));

  for (unsigned i = 0; i < the_blob.gl_pathc; ++i)
    {
      const char* globbed = the_blob.gl_pathv[i];
      struct stat st;

      if (access (globbed, X_OK) == 0
          && stat (globbed, &st) == 0
          && S_ISREG (st.st_mode)) // see find_executable()
        {
          // Need to call resolve_path here, in order to path-expand
          // patterns like process("stap*").  Otherwise it may go through
          // to the next round of expansion as ("stap"), leading to a $PATH
          // search that's not consistent with the glob search already done.
          string canononicalized = resolve_path (globbed);

          // The canonical names can result in duplication, for example
          // having followed symlinks that are common with shared libraries,
          // so we use a set for unique results.
          globs.insert(canononicalized);
        }
    }

  globfree (& the_blob);
  return globs;
}

static bool
resolve_library_by_path(base_query & q,
                        set<string> const & visited_libraries,
                        probe * base,
                        probe_point * location,
                        literal_map_t const & parameters,
                        vector<derived_probe *> & finished_results)
{
  size_t results_pre = finished_results.size();
  systemtap_session & sess = q.sess;
  dwflpp & dw = q.dw;

  string lib;
  if (!location->from_globby_comp(TOK_LIBRARY) && q.has_library
      && !visited_libraries.empty()
      && q.get_string_param(parameters, TOK_LIBRARY, lib))
    {
      // The library didn't fit any DT_NEEDED libraries. As a last effort,
      // let's try to look for the library directly.

      if (contains_glob_chars (lib))
        {
          // Evaluate glob here, and call derive_probes recursively with each match.
          set<string> globs = glob_executable (lib);
          for (set<string>::const_iterator it = globs.begin();
               it != globs.end(); ++it)
            {
              assert_no_interrupts();

              const string& globbed = *it;
              if (sess.verbose > 1)
                clog << _F("Expanded library(\"%s\") to library(\"%s\")",
                           lib.c_str(), globbed.c_str()) << endl;

              probe *new_base = build_library_probe(dw, globbed,
                                                    base, location);

              // We override "optional = true" here, as if the
              // wildcarded probe point was given a "?" suffix.

              // This is because wildcard probes will be expected
              // by users to apply only to some subset of the
              // matching binaries, in the sense of "any", rather
              // than "all", sort of similarly how
              // module("*").function("...") patterns work.

              derive_probes (sess, new_base, finished_results,
                             true /* NB: not location->optional */ );
            }
        }
      else
        {
          string resolved_lib = find_executable(lib, sess.sysroot, sess.sysenv,
                                                "LD_LIBRARY_PATH");
          if (resolved_lib.find('/') != string::npos)
            {
              probe *new_base = build_library_probe(dw, resolved_lib,
                                                    base, location);
              derive_probes(sess, new_base, finished_results);
              if (lib.find('/') == string::npos)
                sess.print_warning(_F("'%s' is not a needed library of '%s'. "
                                      "Specify the full path to squelch this warning.",
                                      resolved_lib.c_str(), dw.module_name.c_str()));
            }
          else
            {
              // Otherwise, let's suggest from the DT_NEEDED libraries
              string sugs = levenshtein_suggest(lib, visited_libraries, 5);
              if (!sugs.empty())
                throw SEMANTIC_ERROR (_NF("no match (similar library: %s)",
                                          "no match (similar libraries: %s)",
                                          sugs.find(',') == string::npos,
                                          sugs.c_str()));
            }
        }
    }

  return results_pre != finished_results.size();
}

void
dwarf_builder::build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     literal_map_t const & parameters,
		     vector<derived_probe *> & finished_results)
{
  // NB: the kernel/user dwlfpp objects are long-lived.
  // XXX: but they should be per-session, as this builder object
  // may be reused if we try to cross-instrument multiple targets.

  dwflpp* dw = 0;
  literal_map_t filled_parameters = parameters;

  string module_name;
  int64_t proc_pid;
  if (has_null_param (parameters, TOK_KERNEL))
    {
      dw = get_kern_dw(sess, "kernel");
    }
  else if (get_param (parameters, TOK_MODULE, module_name))
    {
      // If not a full path was given, then it's an in-tree module. Replace any
      // dashes with underscores.
      if (!is_fully_resolved(module_name, sess.sysroot, sess.sysenv))
        {
          size_t dash_pos = 0;
          while((dash_pos=module_name.find('-'))!=string::npos)
            module_name.replace(int(dash_pos),1,"_");
          filled_parameters[TOK_MODULE] = new literal_string(module_name);
        }

      // NB: glob patterns get expanded later, during the offline
      // elfutils module listing.
      dw = get_kern_dw(sess, module_name);
    }
  else if (has_param(filled_parameters, TOK_PROCESS))
      {
        // NB: module_name is not yet set!

      if(has_null_param(filled_parameters, TOK_PROCESS))
        {
          string file;
          try
            {
              file = sess.cmd_file();
            }
          catch (const semantic_error& e)
            {
              if(sess.target_pid)
                throw SEMANTIC_ERROR(_("invalid -x pid for unspecified process"
                                       " probe [man stapprobes]"), NULL, NULL, &e);
              else
                throw SEMANTIC_ERROR(_("invalid -c command for unspecified process"
                                     " probe [man stapprobes]"), NULL, NULL, &e);
            }
          if(file.empty())
            throw SEMANTIC_ERROR(_("unspecified process probe is invalid without"
                                   " a -c COMMAND or -x PID [man stapprobes]"));
          module_name = sess.sysroot + file;
          filled_parameters[TOK_PROCESS] = new literal_string(module_name);// this needs to be used in place of the blank map
          // in the case of TOK_MARK we need to modify locations as well   // XXX why?
          if(location->components[0]->functor==TOK_PROCESS &&
             location->components[0]->arg == 0)
            {
              if (sess.target_pid)
                location->components[0]->arg = new literal_number(sess.target_pid);
              else
                location->components[0]->arg = new literal_string(module_name);
            }
        }

      // NB: must specifically handle the classical ("string") form here, to make sure
      // we get the module_name out.
      else if (get_param (parameters, TOK_PROCESS, module_name))
        {
          module_name = sess.sysroot + module_name;
          filled_parameters[TOK_PROCESS] = new literal_string(module_name);
        }

      else if (get_param (parameters, TOK_PROCESS, proc_pid))
        {
          // check that the pid given corresponds to a running process
          string pid_err_msg;
          if (!is_valid_pid(proc_pid, pid_err_msg))
            throw SEMANTIC_ERROR(pid_err_msg);

          string pid_path = string("/proc/") + lex_cast(proc_pid) + "/exe";
          module_name = sess.sysroot + pid_path;

          // in the case of TOK_MARK we need to modify locations as well  // XXX why?
          if(location->components[0]->functor==TOK_PROCESS &&
             location->components[0]->arg == 0)
            location->components[0]->arg = new literal_number(sess.target_pid);
        }

      // PR6456  process("/bin/*")  glob handling
      if (contains_glob_chars (module_name))
        {
          // Expand glob via rewriting the probe-point process("....")
          // parameter, asserted to be the first one.

          assert (location->components.size() > 0);
          assert (location->components[0]->functor == TOK_PROCESS);
          assert (location->components[0]->arg);
          literal_string* lit = dynamic_cast<literal_string*>(location->components[0]->arg);
          assert (lit);

          // Evaluate glob here, and call derive_probes recursively with each match.
          set<string> globs = glob_executable (module_name);
          unsigned results_pre = finished_results.size();
          for (set<string>::const_iterator it = globs.begin();
               it != globs.end(); ++it)
            {
              assert_no_interrupts();

              const string& globbed = *it;

              // synthesize a new probe_point, with the glob-expanded string
              probe_point *pp = new probe_point (*location);

              // PR13338: quote results to prevent recursion
              string eglobbed = escape_glob_chars (globbed);

              if (sess.verbose > 1)
                clog << _F("Expanded process(\"%s\") to process(\"%s\")",
                           module_name.c_str(), eglobbed.c_str()) << endl;
              string eglobbed_tgt = path_remove_sysroot(sess, eglobbed);

              probe_point::component* ppc
                = new probe_point::component (TOK_PROCESS,
                                              new literal_string (eglobbed_tgt),
                                              true /* from_glob */ );
              ppc->tok = location->components[0]->tok; // overwrite [0] slot, pattern matched above
              pp->components[0] = ppc;

              probe* new_probe = new probe (base, pp);

              // We override "optional = true" here, as if the
              // wildcarded probe point was given a "?" suffix.

              // This is because wildcard probes will be expected
              // by users to apply only to some subset of the
              // matching binaries, in the sense of "any", rather
              // than "all", sort of similarly how
              // module("*").function("...") patterns work.

              derive_probes (sess, new_probe, finished_results,
                             true /* NB: not location->optional */ );
            }

          unsigned results_post = finished_results.size();

          // Did we fail to find a function/plt/mark by name? Let's suggest
          // something!
          string func;
          if (results_pre == results_post
              && get_param(filled_parameters, TOK_FUNCTION, func)
              && !func.empty())
            {
              string sugs = suggest_dwarf_functions(sess, modules_seen, func);
              modules_seen.clear();
              if (!sugs.empty())
                throw SEMANTIC_ERROR (_NF("no match (similar function: %s)",
                                          "no match (similar functions: %s)",
                                          sugs.find(',') == string::npos,
                                          sugs.c_str()));
            }
          else if (results_pre == results_post
                   && get_param(filled_parameters, TOK_PLT, func)
                   && !func.empty())
            {
              string sugs = suggest_plt_functions(sess, modules_seen, func);
              modules_seen.clear();
              if (!sugs.empty())
                throw SEMANTIC_ERROR (_NF("no match (similar function: %s)",
                                          "no match (similar functions: %s)",
                                          sugs.find(',') == string::npos,
                                          sugs.c_str()));
            }
          else if (results_pre == results_post
                   && get_param(filled_parameters, TOK_MARK, func)
                   && !func.empty())
            {
              string provider;
              get_param(filled_parameters, TOK_PROVIDER, provider);

              string sugs = suggest_marks(sess, modules_seen, func, provider);
              modules_seen.clear();
              if (!sugs.empty())
                throw SEMANTIC_ERROR (_NF("no match (similar mark: %s)",
                                          "no match (similar marks: %s)",
                                          sugs.find(',') == string::npos,
                                          sugs.c_str()));
            }

          return; // avoid falling through
        }

      // PR13338: unquote glob results
      module_name = unescape_glob_chars (module_name);
      user_path = find_executable (module_name, "", sess.sysenv); // canonicalize it
      if (!is_fully_resolved(user_path, "", sess.sysenv))
        throw SEMANTIC_ERROR(_F("cannot find executable '%s'",
                                user_path.c_str()));

      // if the executable starts with "#!", we look for the interpreter of the script
      {
         ifstream script_file (user_path.c_str () );

         if (script_file.good ())
         {
           string line;

           getline (script_file, line);

           if (line.compare (0, 2, "#!") == 0)
           {
              string path_head = line.substr(2);

              // remove white spaces at the beginning of the string
              size_t p2 = path_head.find_first_not_of(" \t");

              if (p2 != string::npos)
              {
                string path = path_head.substr(p2);

                // remove white spaces at the end of the string
                p2 = path.find_last_not_of(" \t\n");
                if (string::npos != p2)
                  path.erase(p2+1);

                // handle "#!/usr/bin/env" redirect
                size_t offset = 0;
                if (path.compare(0, sizeof("/bin/env")-1, "/bin/env") == 0)
                {
                  offset = sizeof("/bin/env")-1;
                }
                else if (path.compare(0, sizeof("/usr/bin/env")-1, "/usr/bin/env") == 0)
                {
                  offset = sizeof("/usr/bin/env")-1;
                }

                if (offset != 0)
                {
                    size_t p3 = path.find_first_not_of(" \t", offset);

                    if (p3 != string::npos)
                    {
                       string env_path = path.substr(p3);
                       user_path = find_executable (env_path, sess.sysroot,
                                                    sess.sysenv);
                    }
                }
                else
                {
                  user_path = find_executable (path, sess.sysroot, sess.sysenv);
                }

                struct stat st;

                if (access (user_path.c_str(), X_OK) == 0
                  && stat (user_path.c_str(), &st) == 0
                  && S_ISREG (st.st_mode)) // see find_executable()
                {
                  if (sess.verbose > 1)
                    clog << _F("Expanded process(\"%s\") to process(\"%s\")",
                               module_name.c_str(), user_path.c_str()) << endl;

                  assert (location->components.size() > 0);
                  assert (location->components[0]->functor == TOK_PROCESS);
                  assert (location->components[0]->arg);
                  literal_string* lit = dynamic_cast<literal_string*>(location->components[0]->arg);
                  assert (lit);

                  // synthesize a new probe_point, with the expanded string
                  probe_point *pp = new probe_point (*location);
                  string user_path_tgt = path_remove_sysroot(sess, user_path);
                  probe_point::component* ppc = new probe_point::component (TOK_PROCESS,
                                                                            new literal_string (user_path_tgt.c_str()));
                  ppc->tok = location->components[0]->tok; // overwrite [0] slot, pattern matched above
                  pp->components[0] = ppc;

                  probe* new_probe = new probe (base, pp);

                  derive_probes (sess, new_probe, finished_results);

                  script_file.close();
                  return;
                }
              }
           }
         }
         script_file.close();
      }

      // If this is a library probe, then target the library module instead. We
      // do this only if the library path is already fully resolved (such as
      // what query_one_library() would have done for us). Otherwise, we resort
      // to iterate_over_libraries.
      if (get_param (parameters, TOK_LIBRARY, user_lib) && !user_lib.empty()
          && is_fully_resolved(user_lib, sess.sysroot, sess.sysenv, "LD_LIBRARY_PATH"))
        module_name = user_lib;
      else
        module_name = user_path; // canonicalize it

      // uretprobes aren't available everywhere
      if (has_null_param(parameters, TOK_RETURN) && !sess.runtime_usermode_p())
        {
          if (kernel_supports_inode_uprobes(sess) &&
              !kernel_supports_inode_uretprobes(sess))
            throw SEMANTIC_ERROR
              (_("process return probes not available [man error::inode-uprobes]"));
        }

      // There is a similar check in pass 4 (buildrun), but it is
      // needed here too to make sure alternatives for optional
      // (? or !) process probes are disposed and/or alternatives
      // are selected.
      if (!sess.runtime_usermode_p())
        check_process_probe_kernel_support(sess);

      // user-space target; we use one dwflpp instance per module name
      // (= program or shared library)
      dw = get_user_dw(sess, module_name);
    }

  assert(dw);

  unsigned results_pre = finished_results.size();

  if (sess.verbose > 3)
    clog << _F("dwarf_builder::build for %s", module_name.c_str()) << endl;

  string dummy_mark_name; // NB: PR10245: dummy value, need not substitute - => __
  if (get_param(parameters, TOK_MARK, dummy_mark_name))
    {
      sdt_query sdtq(base, location, *dw, filled_parameters, finished_results, user_lib);
      dw->iterate_over_modules<base_query>(&query_module, &sdtq);

      // We need to update modules_seen with the modules we've visited
      modules_seen.insert(sdtq.visited_modules.begin(),
                          sdtq.visited_modules.end());

      if (results_pre == finished_results.size()
          && sdtq.has_library && !sdtq.resolved_library
          && resolve_library_by_path (sdtq, sdtq.visited_libraries,
                                      base, location, filled_parameters,
                                      finished_results))
        return;

      // Did we fail to find a mark?
      if (results_pre == finished_results.size()
          && !location->from_globby_comp(TOK_MARK))
        {
          string provider;
          (void) get_param(filled_parameters, TOK_PROVIDER, provider);

          string sugs = suggest_marks(sess, modules_seen, dummy_mark_name, provider);
          modules_seen.clear();
          if (!sugs.empty())
            throw SEMANTIC_ERROR (_NF("no match (similar mark: %s)",
                                      "no match (similar marks: %s)",
                                      sugs.find(',') == string::npos,
                                      sugs.c_str()));
        }

      return;
    }

  dwarf_query q(base, location, *dw, filled_parameters, finished_results, user_path, user_lib);

  // XXX: kernel.statement.absolute is a special case that requires no
  // dwfl processing.  This code should be in a separate builder.
  if (q.has_kernel && q.has_absolute)
    {
      // assert guru mode for absolute probes
      if (! q.base_probe->privileged)
        {
          throw SEMANTIC_ERROR (_("absolute statement probe in unprivileged script; need stap -g"),
                                q.base_probe->tok);
        }

      // For kernel.statement(NUM).absolute probe points, we bypass
      // all the debuginfo stuff: We just wire up a
      // dwarf_derived_probe right here and now.
      dwarf_derived_probe* p =
        new dwarf_derived_probe ("", "", 0, "kernel", "",
                                 q.statement_num_val, q.statement_num_val,
                                 q, 0);
      finished_results.push_back (p);
      sess.unwindsym_modules.insert ("kernel");
      return;
    }

  dw->iterate_over_modules<base_query>(&query_module, &q);

  // We need to update modules_seen with the modules we've visited
  modules_seen.insert(q.visited_modules.begin(),
                      q.visited_modules.end());

  // PR11553 special processing: .return probes requested, but
  // some inlined function instances matched.
  unsigned i_n_r = q.inlined_non_returnable.size();
  unsigned results_post = finished_results.size();
  if (i_n_r > 0)
    {
      if ((results_pre == results_post) && (! sess.suppress_warnings)) // no matches; issue warning
        {
          string quicklist;
          for (set<string>::iterator it = q.inlined_non_returnable.begin();
               it != q.inlined_non_returnable.end();
               it++)
            {
              quicklist += " " + (*it);
              if (quicklist.size() > 80) // heuristic, don't make an overlong report line
                {
                  quicklist += " ...";
                  break;
                }
            }

          sess.print_warning (_NF("cannot probe .return of %u inlined function %s",
                                          "cannot probe .return of %u inlined functions %s",
                                           quicklist.size(), i_n_r, quicklist.c_str()));
          // There will be also a "no matches" semantic error generated.
        }
      if (sess.verbose > 1)
        clog << _NF("skipped .return probe of %u inlined function",
                            "skipped .return probe of %u inlined functions", i_n_r, i_n_r) << endl;
      if ((sess.verbose > 3) || (sess.verbose > 2 && results_pre == results_post)) // issue details with high verbosity
        {
          for (set<string>::iterator it = q.inlined_non_returnable.begin();
               it != q.inlined_non_returnable.end();
               it++)
            clog << (*it) << " ";
          clog << endl;
        }
    } // i_n_r > 0

  if (results_pre == finished_results.size()
      && q.has_library && !q.resolved_library
      && resolve_library_by_path (q, q.visited_libraries,
                                  base, location, filled_parameters,
                                  finished_results))
    return;

  // If we just failed to resolve a function/plt by name, we can suggest
  // something. We only suggest things for probe points that were not
  // synthesized from a glob, i.e. only for 'real' probes. This is also
  // required because modules_seen needs to accumulate across recursive
  // calls for process(glob)[.library(glob)] probes.
  string func;
  if (results_pre == results_post && !location->from_globby_comp(TOK_FUNCTION)
      && get_param(filled_parameters, TOK_FUNCTION, func)
      && !func.empty())
    {
      string sugs = suggest_dwarf_functions(sess, modules_seen, func);
      modules_seen.clear();
      if (!sugs.empty())
        throw SEMANTIC_ERROR (_NF("no match (similar function: %s)",
                                  "no match (similar functions: %s)",
                                  sugs.find(',') == string::npos,
                                  sugs.c_str()));
    }
  else if (results_pre == results_post && !location->from_globby_comp(TOK_PLT)
           && get_param(filled_parameters, TOK_PLT, func)
           && !func.empty())
    {
      string sugs = suggest_plt_functions(sess, modules_seen, func);
      modules_seen.clear();
      if (!sugs.empty())
        throw SEMANTIC_ERROR (_NF("no match (similar function: %s)",
                                  "no match (similar functions: %s)",
                                  sugs.find(',') == string::npos,
                                  sugs.c_str()));
    }
  else if (results_pre != results_post)
    // Something was derived so we won't need to suggest something
    modules_seen.clear();
}

symbol_table::~symbol_table()
{
  delete_map(map_by_addr);
}

void
symbol_table::add_symbol(const char *name, bool weak, bool descriptor,
                         Dwarf_Addr addr, Dwarf_Addr entrypc)
{
  /* Does the target architecture have function descriptors?
     Then we want to filter them out. When seeing a symbol with a name
     starting with '.' we assume it is a regular function pointer and
     not a pointer to a function descriptor. Note that this might create
     duplicates if we also found the function descriptor symbol itself.
     dwfl_module_getsym_info () will have resolved the actual function
     address for us. But we won't know if we see either or both.  */
  if (opd_section != SHN_UNDEF)
    {
      // Map ".sys_foo" to "sys_foo".
      if (name[0] == '.')
	name++;

      // Make sure we don't create duplicate func_info's
      range_t er = map_by_addr.equal_range(addr);
      for (iterator_t it = er.first; it != er.second; ++it)
        if (it->second->name == name)
	  return;
    }

  func_info *fi = new func_info();
  fi->entrypc = entrypc;
  fi->addr = addr;
  fi->name = name;
  fi->weak = weak;
  fi->descriptor = descriptor;

  map_by_name.insert(make_pair(fi->name, fi));
  map_by_addr.insert(make_pair(addr, fi));
}

void
symbol_table::prepare_section_rejection(Dwfl_Module *mod __attribute__ ((unused)))
{
  Dwarf_Addr bias;
  Elf* elf = (dwarf_getelf (dwfl_module_getdwarf (mod, &bias))
              ?: dwfl_module_getelf (mod, &bias));

  GElf_Ehdr ehdr_mem;
  GElf_Ehdr* em = gelf_getehdr (elf, &ehdr_mem);
  if (em == NULL) throw SEMANTIC_ERROR (_("Couldn't get elf header"));

  /* Only old ELFv1 PPC64 ABI have function descriptors.  */
  opd_section = SHN_UNDEF;
  if (em->e_machine != EM_PPC64 || (em->e_flags & EF_PPC64_ABI) == 2)
    return;

  /*
   * The .opd section contains function descriptors that can look
   * just like function entry points.  For example, there's a function
   * descriptor called "do_exit" that links to the entry point ".do_exit".
   * Reject all symbols in .opd.
   */
  Elf_Scn* scn = 0;
  size_t shstrndx;

  if (!elf)
    return;
  if (elf_getshdrstrndx (elf, &shstrndx) != 0)
    return;
  while ((scn = elf_nextscn(elf, scn)) != NULL)
    {
      GElf_Shdr shdr_mem;
      GElf_Shdr *shdr = gelf_getshdr(scn, &shdr_mem);
      if (!shdr)
        continue;
      const char *name = elf_strptr(elf, shstrndx, shdr->sh_name);
      if (!strcmp(name, ".opd"))
        {
          opd_section = elf_ndxscn(scn);
          return;
        }
    }
}

bool
symbol_table::reject_section(GElf_Word section)
{
  if (section == SHN_UNDEF || section == opd_section)
    return true;
  return false;
}

enum info_status
symbol_table::get_from_elf()
{
  Dwfl_Module *mod = mod_info->mod;
  int syments = dwfl_module_getsymtab(mod);
  assert(syments);
  prepare_section_rejection(mod);
  Dwarf_Addr bias;
  Elf* elf = (dwarf_getelf (dwfl_module_getdwarf (mod, &bias))
              ?: dwfl_module_getelf (mod, &bias));

  GElf_Ehdr ehdr_mem;
  GElf_Ehdr* em = gelf_getehdr (elf, &ehdr_mem);
  if (em == NULL) throw SEMANTIC_ERROR (_("Couldn't get elf header"));

  for (int i = 1; i < syments; ++i)
    {
      GElf_Sym sym;
      GElf_Word section;
      const char *name;
      GElf_Addr addr;
      bool reject;

/* Note that dwfl_module_getsym does adjust the sym.st_value but doesn't
   try to resolve it to a function address.  dwfl_module_getsym_info leaves
   the st_value in tact (no adjustment applied) and returns the fully
   resolved address separately. In that case we can simply reject the
   symbol if it is SHN_UNDEF and don't need to call reject_section which
   does extra checks to see whether the address fall in an architecture
   specific descriptor table (which will never be the case when using the
   new dwfl_module_getsym_info).  dwfl_module_getsym will only provide us
   with the (adjusted) st_value of the symbol, which might point into a
   function descriptor table. So in that case we still have to call
   reject_section. */
#if _ELFUTILS_PREREQ (0, 158)
      name = dwfl_module_getsym_info (mod, i, &sym, &addr, &section,
				      NULL, NULL);
      reject = section == SHN_UNDEF;
#else
      name = dwfl_module_getsym (mod, i, &sym, &section);
      addr = sym.st_value;
      reject = reject_section(section);
#endif
     /*
      * For ELF ABI v2 on PPC64 LE, we need to adjust sym.st_value corresponding
      * to the bits of sym.st_other. These bits will tell us what's the offset
      * of the local entry point from the global entry point.
      *
      * st_other field is currently only used with ABIv2 on ppc64
      */
      Dwarf_Addr entrypc = addr;
      if ((em->e_machine == EM_PPC64) && ((em->e_flags & EF_PPC64_ABI) == 2)
          && (GELF_ST_TYPE(sym.st_info) == STT_FUNC) && sym.st_other)
        entrypc += PPC64_LOCAL_ENTRY_OFFSET(sym.st_other);

      if (name && GELF_ST_TYPE(sym.st_info) == STT_FUNC)
        add_symbol(name, (GELF_ST_BIND(sym.st_info) == STB_WEAK),
                   reject, addr, entrypc);
      if (name && GELF_ST_TYPE(sym.st_info) == STT_OBJECT
               && GELF_ST_BIND(sym.st_info) == STB_GLOBAL)
        globals[name] = addr;
      if (name && GELF_ST_TYPE(sym.st_info) == STT_OBJECT
               && GELF_ST_BIND(sym.st_info) == STB_LOCAL)
        locals[name] = addr;
    }
  return info_present;
}

func_info *
symbol_table::get_func_containing_address(Dwarf_Addr addr)
{
  iterator_t iter = map_by_addr.upper_bound(addr);
  if (iter == map_by_addr.begin())
    return NULL;
  else
    return (--iter)->second;
}

func_info *
symbol_table::get_first_func()
{
  iterator_t iter = map_by_addr.begin();
  return (iter)->second;
}

/* Note this function filters out any symbols that are "rejected" because
   they are "descriptor" function symbols or SHN_UNDEF symbols. */
set <func_info*>
symbol_table::lookup_symbol(const string& name)
{
  set<func_info*> fis;
  pair <multimap<string, func_info*>::iterator, multimap<string, func_info*>::iterator> ret;
  ret = map_by_name.equal_range(name);
  for (multimap<string, func_info*>::iterator it = ret.first; it != ret.second; ++it)
    if (! it->second->descriptor)
      fis.insert(it->second);
  return fis;
}

/* Filters out the same "descriptor" or SHN_UNDEF symbols as
   symbol_table::lookup_symbol.  */
set <Dwarf_Addr>
symbol_table::lookup_symbol_address(const string& name)
{
  set <Dwarf_Addr> addrs;
  set <func_info*> fis = lookup_symbol(name);

  for (set<func_info*>::iterator it=fis.begin(); it!=fis.end(); ++it)
    addrs.insert((*it)->addr);

  return addrs;
}

// This is the kernel symbol table.  The kernel macro cond_syscall creates
// a weak symbol for each system call and maps it to sys_ni_syscall.
// For system calls not implemented elsewhere, this weak symbol shows up
// in the kernel symbol table.  Following the precedent of dwarfful stap,
// we refuse to consider such symbols.  Here we delete them from our
// symbol table.
// TODO: Consider generalizing this and/or making it part of blacklist
// processing.
void
symbol_table::purge_syscall_stubs()
{
  set<Dwarf_Addr> addrs = lookup_symbol_address("sys_ni_syscall");
  if (addrs.empty())
    return;

  /* Highly unlikely that multiple symbols named "sys_ni_syscall" may exist */
  if (addrs.size() > 1)
    cerr << _("Multiple 'sys_ni_syscall' symbols found.\n");
  Dwarf_Addr stub_addr = * addrs.begin();

  range_t purge_range = map_by_addr.equal_range(stub_addr);
  for (iterator_t iter = purge_range.first;
       iter != purge_range.second;
       )
    {
      func_info *fi = iter->second;
      if (fi->weak && fi->name != "sys_ni_syscall")
        {
          map_by_name.erase(fi->name);
          map_by_addr.erase(iter++);
          delete fi;
        }
      else
        iter++;
    }
}

void
module_info::get_symtab()
{
  if (symtab_status != info_unknown)
    return;

  sym_table = new symbol_table(this);
  if (!elf_path.empty())
    {
      symtab_status = sym_table->get_from_elf();
    }
  else
    {
      assert(name == TOK_KERNEL);
      symtab_status = info_absent;
      cerr << _("Error: Cannot find vmlinux.") << endl;;
    }
  if (symtab_status == info_absent)
    {
      delete sym_table;
      sym_table = NULL;
      return;
    }

  if (name == TOK_KERNEL)
    sym_table->purge_syscall_stubs();
}

// update_symtab reconciles data between the elf symbol table and the dwarf
// function enumeration.  It updates the symbol table entries with the dwarf
// die that describes the function, which also signals to query_module_symtab
// that a statement probe isn't needed.  In return, it also adds aliases to the
// function table for names that share the same addr/die.
void
module_info::update_symtab(cu_function_cache_t *funcs)
{
  if (!sym_table)
    return;

  cu_function_cache_t new_funcs;

  for (cu_function_cache_t::iterator func = funcs->begin();
       func != funcs->end(); func++)
    {
      // optimization: inlines will never be in the symbol table
      if (dwarf_func_inline(&func->second) != 0)
        {
          inlined_funcs.insert(func->first);
          continue;
        }

      // We need to make additional efforts to match mangled elf names to dwarf
      // too.  DW_AT_linkage_name (or w/ MIPS) can help, but that's sometimes
      // missing, so we may also need to try matching by address.  See also the
      // notes about _Z in dwflpp::iterate_over_functions().
      const string& name = dwarf_linkage_name(&func->second) ?: func->first;

      set<func_info*> fis = sym_table->lookup_symbol(name);
      if (fis.empty())
        continue;

      for (set<func_info*>::iterator fi = fis.begin(); fi!=fis.end(); ++fi)
        {
          // iterate over all functions at the same address
          symbol_table::range_t er = sym_table->map_by_addr.equal_range((*fi)->addr);

          for (symbol_table::iterator_t it = er.first; it != er.second; ++it)
            {
              // update this function with the dwarf die
              it->second->die = func->second;

              // if this function is a new alias, then
              // save it to merge into the function cache
              if (it->second != *fi)
                new_funcs.insert(make_pair(it->second->name, it->second->die));
            }
        }
    }

  // add all discovered aliases back into the function cache
  // NB: this won't replace any names that dwarf may have already found
  funcs->insert(new_funcs.begin(), new_funcs.end());
}

module_info::~module_info()
{
  if (sym_table)
    delete sym_table;
}

// ------------------------------------------------------------------------
// user-space probes
// ------------------------------------------------------------------------


struct uprobe_derived_probe_group: public generic_dpg<uprobe_derived_probe>
{
private:
  string make_pbm_key (uprobe_derived_probe* p) {
    return p->path + "|" + p->module + "|" + p->section + "|" + lex_cast(p->pid);
  }

  void emit_module_maxuprobes (systemtap_session& s);

  // Using our own utrace-based uprobes
  void emit_module_utrace_decls (systemtap_session& s);
  void emit_module_utrace_init (systemtap_session& s);
  void emit_module_utrace_exit (systemtap_session& s);

  // Using the upstream inode-based uprobes
  void emit_module_inode_decls (systemtap_session& s);
  void emit_module_inode_init (systemtap_session& s);
  void emit_module_inode_refresh (systemtap_session& s);
  void emit_module_inode_exit (systemtap_session& s);

  // Using the dyninst backend (via stapdyn)
  void emit_module_dyninst_decls (systemtap_session& s);
  void emit_module_dyninst_init (systemtap_session& s);
  void emit_module_dyninst_exit (systemtap_session& s);

public:
  void emit_module_decls (systemtap_session& s);
  void emit_module_init (systemtap_session& s);
  void emit_module_refresh (systemtap_session& s);
  void emit_module_exit (systemtap_session& s);

  // on-the-fly only supported for inode-uprobes
  bool otf_supported (systemtap_session& s)
    { return !s.runtime_usermode_p()
             && kernel_supports_inode_uprobes(s); }

  // workqueue manipulation is safe in uprobes
  bool otf_safe_context (systemtap_session& s)
    { return otf_supported(s); }
};


void
uprobe_derived_probe::join_group (systemtap_session& s)
{
  if (! s.uprobe_derived_probes)
    s.uprobe_derived_probes = new uprobe_derived_probe_group ();
  s.uprobe_derived_probes->enroll (this);
  this->group = s.uprobe_derived_probes;

  if (s.runtime_usermode_p())
    enable_dynprobes(s);
  else
    enable_task_finder(s);

  // Ask buildrun.cxx to build extra module if needed, and
  // signal staprun to load that module.  If we're using the builtin
  // inode-uprobes, we still need to know that it is required.
  s.need_uprobes = true;
}


void
uprobe_derived_probe::getargs(std::list<std::string> &arg_set) const
{
  dwarf_derived_probe::getargs(arg_set);
  arg_set.insert(arg_set.end(), args.begin(), args.end());
}


void
uprobe_derived_probe::saveargs(int nargs)
{
  for (int i = 1; i <= nargs; i++)
    args.push_back("$arg" + lex_cast (i) + ":long");
}


void
uprobe_derived_probe::emit_privilege_assertion (translator_output* o)
{
  // These probes are allowed for unprivileged users, but only in the
  // context of processes which they own.
  emit_process_owner_assertion (o);
}


struct uprobe_builder: public derived_probe_builder
{
  uprobe_builder() {}
  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     literal_map_t const & parameters,
		     vector<derived_probe *> & finished_results)
  {
    int64_t process, address;

    if (kernel_supports_inode_uprobes(sess))
      throw SEMANTIC_ERROR (_("absolute process probes not available [man error::inode-uprobes]"));

    bool b1 = get_param (parameters, TOK_PROCESS, process);
    (void) b1;
    bool b2 = get_param (parameters, TOK_STATEMENT, address);
    (void) b2;
    bool rr = has_null_param (parameters, TOK_RETURN);
    assert (b1 && b2); // by pattern_root construction

    finished_results.push_back(new uprobe_derived_probe(base, location, process, address, rr));
  }
};


void
uprobe_derived_probe_group::emit_module_maxuprobes (systemtap_session& s)
{
  // We'll probably need at least this many:
  unsigned minuprobes = probes.size();
  // .. but we don't want so many that .bss is inflated (PR10507):
  unsigned uprobesize = 64;
  unsigned maxuprobesmem = 10*1024*1024; // 10 MB
  unsigned maxuprobes = maxuprobesmem / uprobesize;

  // Let's choose a value on the geometric middle.  This should end up
  // between minuprobes and maxuprobes.  It's OK if this number turns
  // out to be < minuprobes or > maxuprobes.  At worst, we get a
  // run-time error of one kind (too few: missed uprobe registrations)
  // or another (too many: vmalloc errors at module load time).
  unsigned default_maxuprobes = (unsigned)sqrt((double)minuprobes * (double)maxuprobes);

  s.op->newline() << "#ifndef MAXUPROBES";
  s.op->newline() << "#define MAXUPROBES " << default_maxuprobes;
  s.op->newline() << "#endif";
}


void
uprobe_derived_probe_group::emit_module_utrace_decls (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- utrace uprobes ---- */";
  // If uprobes isn't in the kernel, pull it in from the runtime.

  s.op->newline() << "#if defined(CONFIG_UPROBES) || defined(CONFIG_UPROBES_MODULE)";
  s.op->newline() << "#include <linux/uprobes.h>";
  s.op->newline() << "#else";
  s.op->newline() << "#include \"linux/uprobes/uprobes.h\"";
  s.op->newline() << "#endif";
  s.op->newline() << "#ifndef UPROBES_API_VERSION";
  s.op->newline() << "#define UPROBES_API_VERSION 1";
  s.op->newline() << "#endif";

  emit_module_maxuprobes (s);

  // Forward decls
  s.op->newline() << "#include \"linux/uprobes-common.h\"";

  // In .bss, the shared pool of uprobe/uretprobe structs.  These are
  // too big to embed in the initialized .data stap_uprobe_spec array.
  // XXX: consider a slab cache or somesuch for stap_uprobes
  s.op->newline() << "static struct stap_uprobe stap_uprobes [MAXUPROBES];";
  s.op->newline() << "static DEFINE_MUTEX(stap_uprobes_lock);"; // protects against concurrent registration/unregistration

  s.op->assert_0_indent();

  // Assign task-finder numbers as we build up the stap_uprobe_tf table.
  // This means we process probes[] in two passes.
  map <string,unsigned> module_index;
  unsigned module_index_ctr = 0;

  // not const since embedded task_finder_target struct changes
  s.op->newline() << "static struct stap_uprobe_tf stap_uprobe_finders[] = {";
  s.op->indent(1);
  for (unsigned i=0; i<probes.size(); i++)
    {
      uprobe_derived_probe *p = probes[i];
      string pbmkey = make_pbm_key (p);
      if (module_index.find (pbmkey) == module_index.end())
        {
          module_index[pbmkey] = module_index_ctr++;

          s.op->newline() << "{";
          // NB: it's essential that make_pbm_key() use all of and
          // only the same fields as we're about to emit.
          s.op->line() << " .finder={";
          s.op->line() << "  .purpose=\"uprobes\",";
          if (p->pid != 0)
            s.op->line() << " .pid=" << p->pid << ",";

          if (p->section == "") // .statement(addr).absolute
            s.op->line() << " .callback=&stap_uprobe_process_found,";
          else if (p->section == ".absolute") // proxy for ET_EXEC -> exec()'d program
            {
              s.op->line() << " .procname=" << lex_cast_qstring(p->module) << ",";
              s.op->line() << " .callback=&stap_uprobe_process_found,";
            }
	  else if (p->section != ".absolute") // ET_DYN
            {
	      if (p->has_library)
	        s.op->line() << " .procname=\"" << p->path << "\", ";
              s.op->line() << " .mmap_callback=&stap_uprobe_mmap_found, ";
              s.op->line() << " .munmap_callback=&stap_uprobe_munmap_found, ";
              s.op->line() << " .callback=&stap_uprobe_process_munmap,";
            }
          s.op->line() << " },";
          if (p->module != "")
            s.op->line() << " .pathname=" << lex_cast_qstring(p->module) << ", ";
          s.op->line() << " },";
        }
      else
        { } // skip it in this pass, already have a suitable stap_uprobe_tf slot for it.
    }
  s.op->newline(-1) << "};";

  s.op->assert_0_indent();

  unsigned pci;
  for (pci=0; pci<probes.size(); pci++)
    {
      // List of perf counters used by each probe
      // This list is an index into struct stap_perf_probe,
      uprobe_derived_probe *p = probes[pci];
      std::set<string>::iterator pcii;
      s.op->newline() << "long perf_counters_" + lex_cast(pci) + "[] = {";
      for (pcii = p->perf_counter_refs.begin();
	   pcii != p->perf_counter_refs.end(); pcii++)
	{
          std::vector<std::pair<std::string,std::string> >::iterator it;
	  unsigned i = 0;
	  // Find the associated perf.counter probe
	  for (it=s.perf_counters.begin() ;
	       it != s.perf_counters.end(); it++, i++)
	    if ((*it).first == (*pcii))
	      break;
	  s.op->line() << lex_cast(i) << ", ";
	}
      s.op->newline() << "};";
    }

   // NB: read-only structure
  s.op->newline() << "static const struct stap_uprobe_spec stap_uprobe_specs [] = {";
  s.op->indent(1);
  for (unsigned i =0; i<probes.size(); i++)
    {
      uprobe_derived_probe* p = probes[i];
      s.op->newline() << "{";
      string key = make_pbm_key (p);
      unsigned value = module_index[key];
      if (value != 0)
        s.op->line() << " .tfi=" << value << ",";
      s.op->line() << " .address=(unsigned long)0x" << hex << p->addr << dec << "ULL,";
      s.op->line() << " .probe=" << common_probe_init (p) << ",";

      if (p->sdt_semaphore_addr != 0)
        s.op->line() << " .sdt_sem_offset=(unsigned long)0x"
                     << hex << p->sdt_semaphore_addr << dec << "ULL,";

      // XXX: don't bother emit if array is empty
      s.op->line() << " .perf_counters_dim=ARRAY_SIZE(perf_counters_" << lex_cast(i) << "),";
      // List of perf counters used by a probe from above
      s.op->line() << " .perf_counters=perf_counters_" + lex_cast(i) + ",";

      if (p->has_return)
        s.op->line() << " .return_p=1,";
      s.op->line() << " },";
    }
  s.op->newline(-1) << "};";

  s.op->assert_0_indent();

  s.op->newline() << "static void enter_uprobe_probe (struct uprobe *inst, struct pt_regs *regs) {";
  s.op->newline(1) << "struct stap_uprobe *sup = container_of(inst, struct stap_uprobe, up);";
  s.op->newline() << "const struct stap_uprobe_spec *sups = &stap_uprobe_specs [sup->spec_index];";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sups->probe",
				 "stp_probe_type_uprobe");
  s.op->newline() << "if (sup->spec_index < 0 || "
                  << "sup->spec_index >= " << probes.size() << ") {";
  s.op->newline(1) << "_stp_error (\"bad spec_index %d (max " << probes.size()
		   << "): %s\", sup->spec_index, c->probe_point);";
  s.op->newline() << "goto probe_epilogue;";
  s.op->newline(-1) << "}";
  s.op->newline() << "c->uregs = regs;";
  s.op->newline() << "c->user_mode_p = 1;";

  // Make it look like the IP is set as it would in the actual user
  // task when calling real probe handler. Reset IP regs on return, so
  // we don't confuse uprobes. PR10458
  s.op->newline() << "{";
  s.op->indent(1);
  s.op->newline() << "unsigned long uprobes_ip = REG_IP(c->uregs);";
  s.op->newline() << "SET_REG_IP(regs, inst->vaddr);";
  s.op->newline() << "(*sups->probe->ph) (c);";
  s.op->newline() << "SET_REG_IP(regs, uprobes_ip);";
  s.op->newline(-1) << "}";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline(-1) << "}";

  s.op->newline() << "static void enter_uretprobe_probe (struct uretprobe_instance *inst, struct pt_regs *regs) {";
  s.op->newline(1) << "struct stap_uprobe *sup = container_of(inst->rp, struct stap_uprobe, urp);";
  s.op->newline() << "const struct stap_uprobe_spec *sups = &stap_uprobe_specs [sup->spec_index];";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sups->probe",
				 "stp_probe_type_uretprobe");
  s.op->newline() << "c->ips.ri = inst;";
  s.op->newline() << "if (sup->spec_index < 0 || "
                  << "sup->spec_index >= " << probes.size() << ") {";
  s.op->newline(1) << "_stp_error (\"bad spec_index %d (max " << probes.size()
		   << "): %s\", sup->spec_index, c->probe_point);";
  s.op->newline() << "goto probe_epilogue;";
  s.op->newline(-1) << "}";

  s.op->newline() << "c->uregs = regs;";
  s.op->newline() << "c->user_mode_p = 1;";

  // Make it look like the IP is set as it would in the actual user
  // task when calling real probe handler. Reset IP regs on return, so
  // we don't confuse uprobes. PR10458
  s.op->newline() << "{";
  s.op->indent(1);
  s.op->newline() << "unsigned long uprobes_ip = REG_IP(c->uregs);";
  s.op->newline() << "SET_REG_IP(regs, inst->ret_addr);";
  s.op->newline() << "(*sups->probe->ph) (c);";
  s.op->newline() << "SET_REG_IP(regs, uprobes_ip);";
  s.op->newline(-1) << "}";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline(-1) << "}";

  s.op->newline();
  s.op->newline() << "#include \"linux/uprobes-common.c\"";
  s.op->newline();
}


void
uprobe_derived_probe_group::emit_module_utrace_init (systemtap_session& s)
{
  if (probes.empty()) return;

  s.op->newline() << "/* ---- utrace uprobes ---- */";

  s.op->newline() << "for (j=0; j<MAXUPROBES; j++) {";
  s.op->newline(1) << "struct stap_uprobe *sup = & stap_uprobes[j];";
  s.op->newline() << "sup->spec_index = -1;"; // free slot
  // NB: we assume the rest of the struct (specificaly, sup->up) is
  // initialized to zero.  This is so that we can use
  // sup->up->kdata = NULL for "really free!"  PR 6829.
  s.op->newline(-1) << "}";
  s.op->newline() << "mutex_init (& stap_uprobes_lock);";

  // Set up the task_finders
  s.op->newline() << "for (i=0; i<sizeof(stap_uprobe_finders)/sizeof(stap_uprobe_finders[0]); i++) {";
  s.op->newline(1) << "struct stap_uprobe_tf *stf = & stap_uprobe_finders[i];";
  s.op->newline() << "probe_point = stf->pathname;"; // for error messages; XXX: would prefer pp() or something better
  s.op->newline() << "rc = stap_register_task_finder_target (& stf->finder);";

  // NB: if (rc), there is no need (XXX: nor any way) to clean up any
  // finders already registered, since mere registration does not
  // cause any utrace or memory allocation actions.  That happens only
  // later, once the task finder engine starts running.  So, for a
  // partial initialization requiring unwind, we need do nothing.
  s.op->newline() << "if (rc) break;";

  s.op->newline(-1) << "}";
}


void
uprobe_derived_probe_group::emit_module_utrace_exit (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- utrace uprobes ---- */";

  // NB: there is no stap_unregister_task_finder_target call;
  // important stuff like utrace cleanups are done by
  // __stp_task_finder_cleanup() via stap_stop_task_finder().
  //
  // This function blocks until all callbacks are completed, so there
  // is supposed to be no possibility of any registration-related code starting
  // to run in parallel with our shutdown here.  So we don't need to protect the
  // stap_uprobes[] array with the mutex.

  s.op->newline() << "for (j=0; j<MAXUPROBES; j++) {";
  s.op->newline(1) << "struct stap_uprobe *sup = & stap_uprobes[j];";
  s.op->newline() << "const struct stap_uprobe_spec *sups = &stap_uprobe_specs [sup->spec_index];";
  s.op->newline() << "if (sup->spec_index < 0) continue;"; // free slot

  // PR10655: decrement that ENABLED semaphore
  s.op->newline() << "if (sup->sdt_sem_address) {";
  s.op->newline(1) << "unsigned short sdt_semaphore;"; // NB: fixed size
  s.op->newline() << "pid_t pid = (sups->return_p ? sup->urp.u.pid : sup->up.pid);";
  s.op->newline() << "struct task_struct *tsk;";
  s.op->newline() << "rcu_read_lock();";

  // Do a pid->task_struct* lookup.  For 2.6.24+, this code assumes
  // that the pid is always in the global namespace, not in any
  // private namespace.
  s.op->newline() << "#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)";
  // We'd like to call find_task_by_pid_ns() here, but it isn't
  // exported.  So, we call what it calls...
  s.op->newline() << "  tsk = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);";
  s.op->newline() << "#else";
  s.op->newline() << "  tsk = find_task_by_pid (pid);";
  s.op->newline() << "#endif /* 2.6.24 */";

  s.op->newline() << "if (tsk) {"; // just in case the thing exited while we weren't watching
  s.op->newline(1) << "if (__access_process_vm_noflush(tsk, sup->sdt_sem_address, &sdt_semaphore, sizeof(sdt_semaphore), 0)) {";
  s.op->newline(1) << "sdt_semaphore --;";
  s.op->newline() << "#ifdef DEBUG_UPROBES";
  s.op->newline() << "_stp_dbug (__FUNCTION__,__LINE__, \"-semaphore %#x @ %#lx\\n\", sdt_semaphore, sup->sdt_sem_address);";
  s.op->newline() << "#endif";
  s.op->newline() << "__access_process_vm_noflush(tsk, sup->sdt_sem_address, &sdt_semaphore, sizeof(sdt_semaphore), 1);";
  s.op->newline(-1) << "}";
  // XXX: need to analyze possibility of race condition
  s.op->newline(-1) << "}";
  s.op->newline() << "rcu_read_unlock();";
  s.op->newline(-1) << "}";

  s.op->newline() << "if (sups->return_p) {";
  s.op->newline(1) << "#ifdef DEBUG_UPROBES";
  s.op->newline() << "_stp_dbug (__FUNCTION__,__LINE__, \"-uretprobe spec %d index %d pid %d addr %p\\n\", sup->spec_index, j, sup->up.pid, (void*) sup->up.vaddr);";
  s.op->newline() << "#endif";
  // NB: PR6829 does not change that we still need to unregister at
  // *this* time -- when the script as a whole exits.
  s.op->newline() << "unregister_uretprobe (& sup->urp);";
  s.op->newline(-1) << "} else {";
  s.op->newline(1) << "#ifdef DEBUG_UPROBES";
  s.op->newline() << "_stp_dbug (__FUNCTION__,__LINE__, \"-uprobe spec %d index %d pid %d addr %p\\n\", sup->spec_index, j, sup->up.pid, (void*) sup->up.vaddr);";
  s.op->newline() << "#endif";
  s.op->newline() << "unregister_uprobe (& sup->up);";
  s.op->newline(-1) << "}";

  s.op->newline() << "sup->spec_index = -1;";

  // XXX: uprobe missed counts?

  s.op->newline(-1) << "}";

  s.op->newline() << "mutex_destroy (& stap_uprobes_lock);";
}


void
uprobe_derived_probe_group::emit_module_inode_decls (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- inode uprobes ---- */";
  emit_module_maxuprobes (s);
  s.op->newline() << "#include \"linux/uprobes-inode.c\"";

  // Write the probe handler.
  s.op->newline() << "static int stapiu_probe_handler "
                  << "(struct stapiu_consumer *sup, struct pt_regs *regs) {";
  s.op->newline(1);

  // Since we're sharing the entry function, we have to dynamically choose the probe_type
  string probe_type = "(sup->return_p ? stp_probe_type_uretprobe : stp_probe_type_uprobe)";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sup->probe",
                                 probe_type);

  s.op->newline() << "c->uregs = regs;";
  s.op->newline() << "c->user_mode_p = 1;";
  // NB: IP is already set by stapiu_probe_prehandler in uprobes-inode.c
  s.op->newline() << "(*sup->probe->ph) (c);";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";
  s.op->assert_0_indent();

  // Index of all the modules for which we need inodes.
  map<string, unsigned> module_index;
  unsigned module_index_ctr = 0;

  // Discover and declare targets for each unique path.
  s.op->newline() << "static struct stapiu_target "
                  << "stap_inode_uprobe_targets[] = {";
  s.op->indent(1);
  for (unsigned i=0; i<probes.size(); i++)
    {
      uprobe_derived_probe *p = probes[i];
      const string key = make_pbm_key(p);
      if (module_index.find (key) == module_index.end())
        {
          module_index[key] = module_index_ctr++;
          s.op->newline() << "{";
          s.op->line() << " .finder={";
          s.op->line() << "  .purpose=\"inode-uprobes\",";
          if (p->pid != 0)
            s.op->line() << " .pid=" << p->pid << ",";

          if (p->section == "") // .statement(addr).absolute  XXX?
            s.op->line() << " .callback=&stapiu_process_found,";
          else if (p->section == ".absolute") // proxy for ET_EXEC -> exec()'d program
            {
              s.op->line() << " .procname=" << lex_cast_qstring(p->module) << ",";
              s.op->line() << " .callback=&stapiu_process_found,";
            }
          else if (p->section != ".absolute") // ET_DYN
            {
              if (p->has_library)
                s.op->line() << " .procname=\"" << p->path << "\", ";
              s.op->line() << " .mmap_callback=&stapiu_mmap_found, ";
              s.op->line() << " .munmap_callback=&stapiu_munmap_found, ";
              s.op->line() << " .callback=&stapiu_process_munmap,";
            }
          s.op->line() << " },";
          s.op->line() << " .filename=" << lex_cast_qstring(p->module) << ",";
          s.op->line() << " },";
        }
    }
  s.op->newline(-1) << "};";
  s.op->assert_0_indent();

  // Declare the actual probes.
  unsigned pci;
  for (pci=0; pci<probes.size(); pci++)
    {
      // List of perf counters used by each probe
      // This list is an index into struct stap_perf_probe,
      uprobe_derived_probe *p = probes[pci];
      std::set<string>::iterator pcii;
      s.op->newline() << "long perf_counters_" + lex_cast(pci) + "[] = {";
      for (pcii = p->perf_counter_refs.begin();
	   pcii != p->perf_counter_refs.end(); pcii++)
	{
          vector<std::pair<string,string> >:: iterator it;
	  unsigned i = 0;
	  // Find the associated perf.counter probe
	  for (it=s.perf_counters.begin() ;
	       it != s.perf_counters.end(); it++, i++)
	    if ((*it).first == (*pcii))
	      break;
	  s.op->line() << lex_cast(i) << ", ";
	}
      s.op->newline() << "};";
    }

  s.op->newline() << "static struct stapiu_consumer "
                  << "stap_inode_uprobe_consumers[] = {";
  s.op->indent(1);
  for (unsigned i=0; i<probes.size(); i++)
    {
      uprobe_derived_probe *p = probes[i];
      unsigned index = module_index[make_pbm_key(p)];
      s.op->newline() << "{";
      if (p->has_return)
        s.op->line() << " .return_p=1,";
      s.op->line() << " .target=&stap_inode_uprobe_targets[" << index << "],";
      s.op->line() << " .offset=(loff_t)0x" << hex << p->addr << dec << "ULL,";
      if (p->sdt_semaphore_addr)
        s.op->line() << " .sdt_sem_offset=(loff_t)0x"
                     << hex << p->sdt_semaphore_addr << dec << "ULL,";
      // XXX: don't bother emit if array is empty
      s.op->line() << " .perf_counters_dim=ARRAY_SIZE(perf_counters_" << lex_cast(i) << "),";
      // List of perf counters used by a probe from above
      s.op->line() << " .perf_counters=perf_counters_" + lex_cast(i) + ",";
      s.op->line() << " .probe=" << common_probe_init (p) << ",";
      s.op->line() << " },";
    }
  s.op->newline(-1) << "};";
  s.op->assert_0_indent();
}


void
uprobe_derived_probe_group::emit_module_inode_init (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- inode uprobes ---- */";
  // Let stapiu_init() handle reporting errors by setting probe_point
  // to NULL.
  s.op->newline() << "probe_point = NULL;";
  s.op->newline() << "rc = stapiu_init ("
                  << "stap_inode_uprobe_targets, "
                  << "ARRAY_SIZE(stap_inode_uprobe_targets), "
                  << "stap_inode_uprobe_consumers, "
                  << "ARRAY_SIZE(stap_inode_uprobe_consumers));";
}


void
uprobe_derived_probe_group::emit_module_inode_refresh (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- inode uprobes ---- */";
  s.op->newline() << "stapiu_refresh ("
                  << "stap_inode_uprobe_targets, "
                  << "ARRAY_SIZE(stap_inode_uprobe_targets));";
}


void
uprobe_derived_probe_group::emit_module_inode_exit (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- inode uprobes ---- */";
  s.op->newline() << "stapiu_exit ("
                  << "stap_inode_uprobe_targets, "
                  << "ARRAY_SIZE(stap_inode_uprobe_targets), "
                  << "stap_inode_uprobe_consumers, "
                  << "ARRAY_SIZE(stap_inode_uprobe_consumers));";
}


void
uprobe_derived_probe_group::emit_module_dyninst_decls (systemtap_session& s)
{
  if (probes.empty()) return;
  s.op->newline() << "/* ---- dyninst uprobes ---- */";
  emit_module_maxuprobes (s);
  s.op->newline() << "#include \"dyninst/uprobes.h\"";

  // Let the dynprobe_derived_probe_group handle outputting targets
  // and probes. This allows us to merge different types of probes.
  s.op->newline() << "static struct stapdu_probe stapdu_probes[];";
  for (unsigned i = 0; i < probes.size(); i++)
    {
      uprobe_derived_probe *p = probes[i];

      dynprobe_add_uprobe(s, p->module, p->addr, p->sdt_semaphore_addr,
			  (p->has_return ? "STAPDYN_PROBE_FLAG_RETURN" : "0"),
			  common_probe_init(p));
    }
  // loc2c-generated code assumes pt_regs are available, so use this to make
  // sure we always have *something* for it to dereference...
  s.op->newline() << "static struct pt_regs stapdu_dummy_uregs;";

  // Write the probe handler.
  // NB: not static, so dyninst can find it
  s.op->newline() << "int enter_dyninst_uprobe "
                  << "(uint64_t index, struct pt_regs *regs) {";
  s.op->newline(1) << "struct stapdu_probe *sup = &stapdu_probes[index];";

  // Since we're sharing the entry function, we have to dynamically choose the probe_type
  string probe_type = "((sup->flags & STAPDYN_PROBE_FLAG_RETURN) ?"
                      " stp_probe_type_uretprobe : stp_probe_type_uprobe)";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sup->probe",
                                 probe_type);

  s.op->newline() << "c->uregs = regs ?: &stapdu_dummy_uregs;";
  s.op->newline() << "c->user_mode_p = 1;";
  // XXX: once we have regs, check how dyninst sets the IP
  // XXX: the way that dyninst rewrites stuff is probably going to be
  // ...  very confusing to our backtracer (at least if we stay in process)
  s.op->newline() << "(*sup->probe->ph) (c);";
  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";
  s.op->newline() << "#include \"dyninst/uprobes-regs.c\"";
  s.op->assert_0_indent();
}


void
uprobe_derived_probe_group::emit_module_dyninst_init (systemtap_session& s)
{
  if (probes.empty()) return;

  /* stapdyn handles the dirty work via dyninst */
  s.op->newline() << "/* ---- dyninst uprobes ---- */";
  s.op->newline() << "/* this section left intentionally blank */";
}


void
uprobe_derived_probe_group::emit_module_dyninst_exit (systemtap_session& s)
{
  if (probes.empty()) return;

  /* stapdyn handles the dirty work via dyninst */
  s.op->newline() << "/* ---- dyninst uprobes ---- */";
  s.op->newline() << "/* this section left intentionally blank */";
}


void
uprobe_derived_probe_group::emit_module_decls (systemtap_session& s)
{
  if (s.runtime_usermode_p())
    emit_module_dyninst_decls (s);
  else if (kernel_supports_inode_uprobes (s))
    emit_module_inode_decls (s);
  else
    emit_module_utrace_decls (s);
}


void
uprobe_derived_probe_group::emit_module_init (systemtap_session& s)
{
  if (s.runtime_usermode_p())
    emit_module_dyninst_init (s);
  else if (kernel_supports_inode_uprobes (s))
    emit_module_inode_init (s);
  else
    emit_module_utrace_init (s);
}


void
uprobe_derived_probe_group::emit_module_refresh (systemtap_session& s)
{
  if (!s.runtime_usermode_p() && kernel_supports_inode_uprobes (s))
    emit_module_inode_refresh (s);
}


void
uprobe_derived_probe_group::emit_module_exit (systemtap_session& s)
{
  if (s.runtime_usermode_p())
    emit_module_dyninst_exit (s);
  else if (kernel_supports_inode_uprobes (s))
    emit_module_inode_exit (s);
  else
    emit_module_utrace_exit (s);
}


// ------------------------------------------------------------------------
// Kprobe derived probes
// ------------------------------------------------------------------------

static const string TOK_KPROBE("kprobe");

struct kprobe_derived_probe: public derived_probe
{
  kprobe_derived_probe (systemtap_session& sess,
			vector<derived_probe *> & results,
			probe *base,
			probe_point *location,
			const string& name,
			int64_t stmt_addr,
			bool has_call,
			bool has_return,
			bool has_statement,
			bool has_maxactive,
			bool has_path,
			bool has_library,
			long maxactive_val,
			const string& path,
			const string& library
			);
  string symbol_name;
  Dwarf_Addr addr;
  bool has_call;
  bool has_return;
  bool has_statement;
  bool has_maxactive;
  bool has_path;
  bool has_library;
  long maxactive_val;
  string path;
  string library;
  bool access_var;
  void printsig (std::ostream &o) const;
  void join_group (systemtap_session& s);
};

struct kprobe_derived_probe_group: public derived_probe_group
{
private:
  multimap<string,kprobe_derived_probe*> probes_by_module;
  typedef multimap<string,kprobe_derived_probe*>::iterator p_b_m_iterator;

public:
  void enroll (kprobe_derived_probe* probe);
  void emit_module_decls (systemtap_session& s);
  void emit_module_init (systemtap_session& s);
  void emit_module_exit (systemtap_session& s);
};

struct kprobe_var_expanding_visitor: public var_expanding_visitor
{
  systemtap_session& sess;
  block *add_block;
  block *add_call_probe; // synthesized from .return probes with saved $vars
  bool add_block_tid, add_call_probe_tid;
  bool has_return;

  kprobe_var_expanding_visitor(systemtap_session& sess, bool has_return):
    sess(sess), add_block(NULL), add_call_probe(NULL),
    add_block_tid(false), add_call_probe_tid(false),
    has_return(has_return) {}

  void visit_entry_op (entry_op* e);
};


kprobe_derived_probe::kprobe_derived_probe (systemtap_session& sess,
					    vector<derived_probe *> & results,
					    probe *base,
					    probe_point *location,
					    const string& name,
					    int64_t stmt_addr,
					    bool has_call,
					    bool has_return,
					    bool has_statement,
					    bool has_maxactive,
					    bool has_path,
					    bool has_library,
					    long maxactive_val,
					    const string& path,
					    const string& library
					    ):
  derived_probe (base, location, true /* .components soon rewritten */ ),
  symbol_name (name), addr (stmt_addr), has_call (has_call),
  has_return (has_return), has_statement (has_statement),
  has_maxactive (has_maxactive), has_path (has_path),
  has_library (has_library),
  maxactive_val (maxactive_val),
  path (path), library (library)
{
  this->tok = base->tok;
  this->access_var = false;

#ifndef USHRT_MAX
#define USHRT_MAX 32767
#endif

  // Expansion of $target variables in the probe body produces an error during
  // translate phase, since we're not using debuginfo

  vector<probe_point::component*> comps;
  comps.push_back (new probe_point::component(TOK_KPROBE));

  if (has_statement)
    {
      comps.push_back (new probe_point::component(TOK_STATEMENT,
                                                  new literal_number(addr, true)));
      comps.push_back (new probe_point::component(TOK_ABSOLUTE));
    }
  else
    {
      size_t pos = name.find(':');
      if (pos != string::npos)
        {
          string module = name.substr(0, pos);
          string function = name.substr(pos + 1);
          comps.push_back (new probe_point::component(TOK_MODULE, new literal_string(module)));
          comps.push_back (new probe_point::component(TOK_FUNCTION, new literal_string(function)));
        }
      else
        comps.push_back (new probe_point::component(TOK_FUNCTION, new literal_string(name)));
    }

  if (has_call)
    comps.push_back (new probe_point::component(TOK_CALL));
  if (has_return)
    comps.push_back (new probe_point::component(TOK_RETURN));
  if (has_maxactive)
    comps.push_back (new probe_point::component(TOK_MAXACTIVE, new literal_number(maxactive_val)));

  kprobe_var_expanding_visitor v (sess, has_return);
  v.replace (this->body);

  // If during target-variable-expanding the probe, we added a new block
  // of code, add it to the start of the probe.
  if (v.add_block)
    this->body = new block(v.add_block, this->body);

  // If when target-variable-expanding the probe, we need to
  // synthesize a sibling function-entry probe.  We don't go through
  // the whole probe derivation business (PR10642) that could lead to
  // wildcard/alias resolution, or for that dwarf-induced duplication.
  if (v.add_call_probe)
    {
      assert (has_return);

      // We temporarily replace base.
      statement* old_body = base->body;
      base->body = v.add_call_probe;

      derived_probe *entry_handler
	= new kprobe_derived_probe (sess, results, base, location, name, 0,
				    true /* has_call */, false /* has_return */,
				    has_statement, has_maxactive, has_path,
				    has_library, maxactive_val, path, library);
      results.push_back (entry_handler);

      base->body = old_body;
    }

  this->sole_location()->components = comps;
}

void kprobe_derived_probe::printsig (ostream& o) const
{
  sole_location()->print (o);
  o << " /* " << " name = " << symbol_name << "*/";
  printsig_nested (o);
}

void kprobe_derived_probe::join_group (systemtap_session& s)
{
  if (! s.kprobe_derived_probes)
	s.kprobe_derived_probes = new kprobe_derived_probe_group ();
  s.kprobe_derived_probes->enroll (this);
  this->group = s.kprobe_derived_probes;
}

void kprobe_derived_probe_group::enroll (kprobe_derived_probe* p)
{
  probes_by_module.insert (make_pair (p->symbol_name, p));
  // probes of same symbol should share single kprobe/kretprobe
}

void
kprobe_derived_probe_group::emit_module_decls (systemtap_session& s)
{
  if (probes_by_module.empty()) return;

  s.op->newline() << "/* ---- kprobe-based probes ---- */";

  // Warn of misconfigured kernels
  s.op->newline() << "#if ! defined(CONFIG_KPROBES)";
  s.op->newline() << "#error \"Need CONFIG_KPROBES!\"";
  s.op->newline() << "#endif";
  s.op->newline();

  s.op->newline() << "#ifndef KRETACTIVE";
  s.op->newline() << "#define KRETACTIVE (max(15,6*(int)num_possible_cpus()))";
  s.op->newline() << "#endif";

  // Forward declare the master entry functions
  s.op->newline() << "static int enter_kprobe2_probe (struct kprobe *inst,";
  s.op->line() << " struct pt_regs *regs);";
  s.op->newline() << "static int enter_kretprobe2_probe (struct kretprobe_instance *inst,";
  s.op->line() << " struct pt_regs *regs);";

  // Emit an array of kprobe/kretprobe pointers
  s.op->newline() << "#if defined(STAPCONF_UNREGISTER_KPROBES)";
  s.op->newline() << "static void * stap_unreg_kprobes2[" << probes_by_module.size() << "];";
  s.op->newline() << "#endif";

  // Emit the actual probe list.

  s.op->newline() << "static struct stap_dwarfless_kprobe {";
  s.op->newline(1) << "union { struct kprobe kp; struct kretprobe krp; } u;";
  s.op->newline() << "#ifdef __ia64__";
  s.op->newline() << "struct kprobe dummy;";
  s.op->newline() << "#endif";
  s.op->newline(-1) << "} stap_dwarfless_kprobes[" << probes_by_module.size() << "];";
  // NB: bss!

  s.op->newline() << "static struct stap_dwarfless_probe {";
  s.op->newline(1) << "const unsigned return_p:1;";
  s.op->newline() << "const unsigned maxactive_p:1;";
  s.op->newline() << "const unsigned optional_p:1;";
  s.op->newline() << "unsigned registered_p:1;";
  s.op->newline() << "const unsigned short maxactive_val;";

  // Function Names are mostly small and uniform enough to justify putting
  // char[MAX]'s into  the array instead of relocated char*'s.

  size_t symbol_string_name_max = 0;
  size_t symbol_string_name_tot = 0;
  for (p_b_m_iterator it = probes_by_module.begin(); it != probes_by_module.end(); it++)
    {
      kprobe_derived_probe* p = it->second;
#define DOIT(var,expr) do {                             \
        size_t var##_size = (expr) + 1;                 \
        var##_max = max (var##_max, var##_size);        \
        var##_tot += var##_size; } while (0)
      DOIT(symbol_string_name, p->symbol_name.size());
#undef DOIT
    }

#define CALCIT(var)                                                     \
	s.op->newline() << "const char " << #var << "[" << var##_name_max << "] ;";

  CALCIT(symbol_string);
#undef CALCIT

  s.op->newline() << "unsigned long address;";
  s.op->newline() << "const struct stap_probe * const probe;";
  s.op->newline(-1) << "} stap_dwarfless_probes[] = {";
  s.op->indent(1);

  for (p_b_m_iterator it = probes_by_module.begin(); it != probes_by_module.end(); it++)
    {
      kprobe_derived_probe* p = it->second;
      s.op->newline() << "{";
      if (p->has_return)
        s.op->line() << " .return_p=1,";

      if (p->has_maxactive)
        {
          s.op->line() << " .maxactive_p=1,";
          assert (p->maxactive_val >= 0 && p->maxactive_val <= USHRT_MAX);
          s.op->line() << " .maxactive_val=" << p->maxactive_val << ",";
        }

      if (p->locations[0]->optional)
        s.op->line() << " .optional_p=1,";

      if (p->has_statement)
        s.op->line() << " .address=(unsigned long)0x" << hex << p->addr << dec << "ULL,";
      else
        s.op->line() << " .symbol_string=\"" << p->symbol_name << "\",";

      s.op->line() << " .probe=" << common_probe_init (p) << ",";
      s.op->line() << " },";
    }

  s.op->newline(-1) << "};";

  // Emit the kprobes callback function
  s.op->newline();
  s.op->newline() << "static int enter_kprobe2_probe (struct kprobe *inst,";
  s.op->line() << " struct pt_regs *regs) {";
  // NB: as of PR5673, the kprobe|kretprobe union struct is in BSS
  s.op->newline(1) << "int kprobe_idx = ((uintptr_t)inst-(uintptr_t)stap_dwarfless_kprobes)/sizeof(struct stap_dwarfless_kprobe);";
  // Check that the index is plausible
  s.op->newline() << "struct stap_dwarfless_probe *sdp = &stap_dwarfless_probes[";
  s.op->line() << "((kprobe_idx >= 0 && kprobe_idx < " << probes_by_module.size() << ")?";
  s.op->line() << "kprobe_idx:0)"; // NB: at least we avoid memory corruption
  // XXX: it would be nice to give a more verbose error though; BUG_ON later?
  s.op->line() << "];";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sdp->probe",
				 "stp_probe_type_kprobe");
  s.op->newline() << "c->kregs = regs;";

  // Make it look like the IP is set as it wouldn't have been replaced
  // by a breakpoint instruction when calling real probe handler. Reset
  // IP regs on return, so we don't confuse kprobes. PR10458
  s.op->newline() << "{";
  s.op->indent(1);
  s.op->newline() << "unsigned long kprobes_ip = REG_IP(c->kregs);";
  s.op->newline() << "SET_REG_IP(regs, (unsigned long) inst->addr);";
  s.op->newline() << "(*sdp->probe->ph) (c);";
  s.op->newline() << "SET_REG_IP(regs, kprobes_ip);";
  s.op->newline(-1) << "}";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";

  // Same for kretprobes
  s.op->newline();
  s.op->newline() << "static int enter_kretprobe2_probe (struct kretprobe_instance *inst,";
  s.op->line() << " struct pt_regs *regs) {";
  s.op->newline(1) << "struct kretprobe *krp = inst->rp;";

  // NB: as of PR5673, the kprobe|kretprobe union struct is in BSS
  s.op->newline() << "int kprobe_idx = ((uintptr_t)krp-(uintptr_t)stap_dwarfless_kprobes)/sizeof(struct stap_dwarfless_kprobe);";
  // Check that the index is plausible
  s.op->newline() << "struct stap_dwarfless_probe *sdp = &stap_dwarfless_probes[";
  s.op->line() << "((kprobe_idx >= 0 && kprobe_idx < " << probes_by_module.size() << ")?";
  s.op->line() << "kprobe_idx:0)"; // NB: at least we avoid memory corruption
  // XXX: it would be nice to give a more verbose error though; BUG_ON later?
  s.op->line() << "];";

  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sdp->probe",
				 "stp_probe_type_kretprobe");
  s.op->newline() << "c->kregs = regs;";
  s.op->newline() << "c->ips.krp.pi = inst;"; // for assisting runtime's backtrace logic

  // Make it look like the IP is set as it wouldn't have been replaced
  // by a breakpoint instruction when calling real probe handler. Reset
  // IP regs on return, so we don't confuse kprobes. PR10458
  s.op->newline() << "{";
  s.op->indent(1);
  s.op->newline() << "unsigned long kprobes_ip = REG_IP(c->kregs);";
  s.op->newline() << "SET_REG_IP(regs, (unsigned long) inst->rp->kp.addr);";
  s.op->newline() << "(*sdp->probe->ph) (c);";
  s.op->newline() << "SET_REG_IP(regs, kprobes_ip);";
  s.op->newline(-1) << "}";

  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";

  s.op->newline() << "#ifdef STAPCONF_KALLSYMS_ON_EACH_SYMBOL";
  s.op->newline() << "static int kprobe_resolve(void *data, const char *name,";
  s.op->newline() << "                          struct module *owner,";
  s.op->newline() << "                          unsigned long val) {";
  s.op->newline(1) << "int i;";
  s.op->newline() << "int *p = (int *) data;";
  s.op->newline() << "for (i=0; i<" << probes_by_module.size()
		  << " && *p > 0; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "if (! sdp->address) {";
  s.op->indent(1);
  s.op->newline() << "const char *colon;";
  s.op->newline() << "if (owner && (colon = strchr(sdp->symbol_string, ':'))) {";
  s.op->indent(1);
  s.op->newline() << "if ((strlen(owner->name) == (colon - sdp->symbol_string))";
  s.op->newline() << "    && (strncmp(sdp->symbol_string, owner->name, colon - sdp->symbol_string) == 0)";
  s.op->newline() << "    && (strcmp(colon + 1, name) == 0)) {";
  s.op->newline(1) << "sdp->address = val;";
  s.op->newline() << "(*p)--;";
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";
  s.op->newline() << "else {";
  s.op->newline(1) << "if (strcmp(sdp->symbol_string, name) == 0) {";
  s.op->newline(1) << "sdp->address = val;";
  s.op->newline() << "(*p)--;";
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";
  s.op->newline() << "return (p > 0) ? 0 : -1;";
  s.op->newline(-1) << "}";
  s.op->newline() << "#endif";
}


void
kprobe_derived_probe_group::emit_module_init (systemtap_session& s)
{
  s.op->newline() << "#ifdef STAPCONF_KALLSYMS_ON_EACH_SYMBOL";
  s.op->newline() << "{";
  s.op->newline(1) << "int p = 0;";
  s.op->newline() << "for (i = 0; i < " << probes_by_module.size() << "; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "if (! sdp->address)";
  s.op->newline(1) << "p++;";
  s.op->newline(-2) << "}";
  s.op->newline() << "kallsyms_on_each_symbol(kprobe_resolve, &p);";
  s.op->newline(-1) << "}";
  s.op->newline() << "#endif";

  s.op->newline() << "for (i=0; i<" << probes_by_module.size() << "; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "struct stap_dwarfless_kprobe *kp = & stap_dwarfless_kprobes[i];";
  s.op->newline() << "void *addr = (void *) sdp->address;";
  s.op->newline() << "const char *symbol_name = addr ? NULL : sdp->symbol_string;";

  s.op->newline() << "#ifdef STAPCONF_KALLSYMS_ON_EACH_SYMBOL";
  s.op->newline() << "if (! addr) {";
  s.op->newline(1) << "sdp->registered_p = 0;";
  s.op->newline() << "if (!sdp->optional_p)";
  s.op->newline(1) << "_stp_warn (\"probe %s registration error (symbol not found)\", probe_point);";
  s.op->newline(-1) << "continue;";
  s.op->newline(-1) << "}";
  s.op->newline() << "#endif";

  s.op->newline() << "probe_point = sdp->probe->pp;"; // for error messages
  s.op->newline() << "if (sdp->return_p) {";
  s.op->newline(1) << "kp->u.krp.kp.addr = addr;";
  s.op->newline() << "#ifdef STAPCONF_KPROBE_SYMBOL_NAME";
  s.op->newline() << "kp->u.krp.kp.symbol_name = (char *) symbol_name;";
  s.op->newline() << "#endif";
  s.op->newline() << "if (sdp->maxactive_p) {";
  s.op->newline(1) << "kp->u.krp.maxactive = sdp->maxactive_val;";
  s.op->newline(-1) << "} else {";
  s.op->newline(1) << "kp->u.krp.maxactive = KRETACTIVE;";
  s.op->newline(-1) << "}";
  s.op->newline() << "kp->u.krp.handler = &enter_kretprobe2_probe;";
  // to ensure safeness of bspcache, always use aggr_kprobe on ia64
  s.op->newline() << "#ifdef __ia64__";
  s.op->newline() << "kp->dummy.addr = kp->u.krp.kp.addr;";
  s.op->newline() << "#ifdef STAPCONF_KPROBE_SYMBOL_NAME";
  s.op->newline() << "kp->dummy.symbol_name = kp->u.krp.kp.symbol_name;";
  s.op->newline() << "#endif";
  s.op->newline() << "kp->dummy.pre_handler = NULL;";
  s.op->newline() << "rc = register_kprobe (& kp->dummy);";
  s.op->newline() << "if (rc == 0) {";
  s.op->newline(1) << "rc = register_kretprobe (& kp->u.krp);";
  s.op->newline() << "if (rc != 0)";
  s.op->newline(1) << "unregister_kprobe (& kp->dummy);";
  s.op->newline(-2) << "}";
  s.op->newline() << "#else";
  s.op->newline() << "rc = register_kretprobe (& kp->u.krp);";
  s.op->newline() << "#endif";
  s.op->newline(-1) << "} else {";
  // to ensure safeness of bspcache, always use aggr_kprobe on ia64
  s.op->newline(1) << "kp->u.kp.addr = addr;";
  s.op->newline() << "#ifdef STAPCONF_KPROBE_SYMBOL_NAME";
  s.op->newline() << "kp->u.kp.symbol_name = (char *) symbol_name;";
  s.op->newline() << "#endif";
  s.op->newline() << "kp->u.kp.pre_handler = &enter_kprobe2_probe;";
  s.op->newline() << "#ifdef __ia64__";
  s.op->newline() << "kp->dummy.pre_handler = NULL;";
  s.op->newline() << "kp->dummy.addr = kp->u.kp.addr;";
  s.op->newline() << "#ifdef STAPCONF_KPROBE_SYMBOL_NAME";
  s.op->newline() << "kp->dummy.symbol_name = kp->u.kp.symbol_name;";
  s.op->newline() << "#endif";
  s.op->newline() << "rc = register_kprobe (& kp->dummy);";
  s.op->newline() << "if (rc == 0) {";
  s.op->newline(1) << "rc = register_kprobe (& kp->u.kp);";
  s.op->newline() << "if (rc != 0)";
  s.op->newline(1) << "unregister_kprobe (& kp->dummy);";
  s.op->newline(-2) << "}";
  s.op->newline() << "#else";
  s.op->newline() << "rc = register_kprobe (& kp->u.kp);";
  s.op->newline() << "#endif";
  s.op->newline(-1) << "}";
  s.op->newline() << "if (rc) {"; // PR6749: tolerate a failed register_*probe.
  s.op->newline(1) << "sdp->registered_p = 0;";
  s.op->newline() << "if (!sdp->optional_p)";
  s.op->newline(1) << "_stp_warn (\"probe %s (address 0x%lx) registration error (rc %d)\", probe_point, (unsigned long) addr, rc);";
  s.op->newline(-1) << "rc = 0;"; // continue with other probes
  // XXX: shall we increment numskipped?
  s.op->newline(-1) << "}";

  s.op->newline() << "else sdp->registered_p = 1;";
  s.op->newline(-1) << "}"; // for loop
}


void
kprobe_derived_probe_group::emit_module_exit (systemtap_session& s)
{
  //Unregister kprobes by batch interfaces.
  s.op->newline() << "#if defined(STAPCONF_UNREGISTER_KPROBES)";
  s.op->newline() << "j = 0;";
  s.op->newline() << "for (i=0; i<" << probes_by_module.size() << "; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "struct stap_dwarfless_kprobe *kp = & stap_dwarfless_kprobes[i];";
  s.op->newline() << "if (! sdp->registered_p) continue;";
  s.op->newline() << "if (!sdp->return_p)";
  s.op->newline(1) << "stap_unreg_kprobes2[j++] = &kp->u.kp;";
  s.op->newline(-2) << "}";
  s.op->newline() << "unregister_kprobes((struct kprobe **)stap_unreg_kprobes2, j);";
  s.op->newline() << "j = 0;";
  s.op->newline() << "for (i=0; i<" << probes_by_module.size() << "; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "struct stap_dwarfless_kprobe *kp = & stap_dwarfless_kprobes[i];";
  s.op->newline() << "if (! sdp->registered_p) continue;";
  s.op->newline() << "if (sdp->return_p)";
  s.op->newline(1) << "stap_unreg_kprobes2[j++] = &kp->u.krp;";
  s.op->newline(-2) << "}";
  s.op->newline() << "unregister_kretprobes((struct kretprobe **)stap_unreg_kprobes2, j);";
  s.op->newline() << "#ifdef __ia64__";
  s.op->newline() << "j = 0;";
  s.op->newline() << "for (i=0; i<" << probes_by_module.size() << "; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "struct stap_dwarfless_kprobe *kp = & stap_dwarfless_kprobes[i];";
  s.op->newline() << "if (! sdp->registered_p) continue;";
  s.op->newline() << "stap_unreg_kprobes2[j++] = &kp->dummy;";
  s.op->newline(-1) << "}";
  s.op->newline() << "unregister_kprobes((struct kprobe **)stap_unreg_kprobes2, j);";
  s.op->newline() << "#endif";
  s.op->newline() << "#endif";

  s.op->newline() << "for (i=0; i<" << probes_by_module.size() << "; i++) {";
  s.op->newline(1) << "struct stap_dwarfless_probe *sdp = & stap_dwarfless_probes[i];";
  s.op->newline() << "struct stap_dwarfless_kprobe *kp = & stap_dwarfless_kprobes[i];";
  s.op->newline() << "if (! sdp->registered_p) continue;";
  s.op->newline() << "if (sdp->return_p) {";
  s.op->newline() << "#if !defined(STAPCONF_UNREGISTER_KPROBES)";
  s.op->newline(1) << "unregister_kretprobe (&kp->u.krp);";
  s.op->newline() << "#endif";
  s.op->newline() << "atomic_add (kp->u.krp.nmissed, skipped_count());";
  s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "if (kp->u.krp.nmissed)";
  s.op->newline(1) << "_stp_warn (\"Skipped due to missed kretprobe/1 on '%s': %d\\n\", sdp->probe->pp, kp->u.krp.nmissed);";
  s.op->newline(-1) << "#endif";
  s.op->newline() << "atomic_add (kp->u.krp.kp.nmissed, skipped_count());";
  s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "if (kp->u.krp.kp.nmissed)";
  s.op->newline(1) << "_stp_warn (\"Skipped due to missed kretprobe/2 on '%s': %lu\\n\", sdp->probe->pp, kp->u.krp.kp.nmissed);";
  s.op->newline(-1) << "#endif";
  s.op->newline(-1) << "} else {";
  s.op->newline() << "#if !defined(STAPCONF_UNREGISTER_KPROBES)";
  s.op->newline(1) << "unregister_kprobe (&kp->u.kp);";
  s.op->newline() << "#endif";
  s.op->newline() << "atomic_add (kp->u.kp.nmissed, skipped_count());";
  s.op->newline() << "#ifdef STP_TIMING";
  s.op->newline() << "if (kp->u.kp.nmissed)";
  s.op->newline(1) << "_stp_warn (\"Skipped due to missed kprobe on '%s': %lu\\n\", sdp->probe->pp, kp->u.kp.nmissed);";
  s.op->newline(-1) << "#endif";
  s.op->newline(-1) << "}";
  s.op->newline() << "#if !defined(STAPCONF_UNREGISTER_KPROBES) && defined(__ia64__)";
  s.op->newline() << "unregister_kprobe (&kp->dummy);";
  s.op->newline() << "#endif";
  s.op->newline() << "sdp->registered_p = 0;";
  s.op->newline(-1) << "}";
}

struct kprobe_builder: public derived_probe_builder
{
public:
  kprobe_builder() {}

  void build_no_more (systemtap_session &s) {}

  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     literal_map_t const & parameters,
		     vector<derived_probe *> & finished_results);
};


void
kprobe_builder::build(systemtap_session & sess,
		      probe * base,
		      probe_point * location,
		      literal_map_t const & parameters,
		      vector<derived_probe *> & finished_results)
{
  string function_string_val, module_string_val;
  string path, library, path_tgt, library_tgt;
  int64_t statement_num_val = 0, maxactive_val = 0;
  bool has_function_str, has_module_str, has_statement_num;
  bool has_absolute, has_call, has_return, has_maxactive;
  bool has_path, has_library;

  has_function_str = get_param(parameters, TOK_FUNCTION, function_string_val);
  has_module_str = get_param(parameters, TOK_MODULE, module_string_val);
  has_call = has_null_param (parameters, TOK_CALL);
  has_return = has_null_param (parameters, TOK_RETURN);
  has_maxactive = get_param(parameters, TOK_MAXACTIVE, maxactive_val);
  has_statement_num = get_param(parameters, TOK_STATEMENT, statement_num_val);
  has_absolute = has_null_param (parameters, TOK_ABSOLUTE);
  has_path = get_param (parameters, TOK_PROCESS, path);
  has_library = get_param (parameters, TOK_LIBRARY, library);

  if (has_path)
    {
      path = find_executable (path, sess.sysroot, sess.sysenv);
      path_tgt = path_remove_sysroot(sess, path);
    }
  if (has_library)
    {
      library = find_executable (library, sess.sysroot, sess.sysenv,
                                 "LD_LIBRARY_PATH");
      library_tgt = path_remove_sysroot(sess, library);
    }

  if (has_function_str)
    {
      if (has_module_str)
        {
	  function_string_val = module_string_val + ":" + function_string_val;
	  derived_probe *dp
	    = new kprobe_derived_probe (sess, finished_results, base,
					location, function_string_val,
					0, has_call, has_return,
					has_statement_num, has_maxactive,
					has_path, has_library, maxactive_val,
					path_tgt, library_tgt);
	  finished_results.push_back (dp);
	}
      else
        {
          vector<string> matches;

          // Simple names can be found directly
          if (function_string_val.find_first_of("*?[") == string::npos)
            {
              if (sess.kernel_functions.count(function_string_val))
                matches.push_back(function_string_val);
            }
          else // Search function name list for matching names
            {
              for (set<string>::const_iterator it = sess.kernel_functions.begin();
                   it != sess.kernel_functions.end(); it++)
                // fnmatch returns zero for matching.
                if (fnmatch(function_string_val.c_str(), it->c_str(), 0) == 0)
                  matches.push_back(*it);
            }

	  for (vector<string>::const_iterator it = matches.begin();
	       it != matches.end(); it++)
	    {
              derived_probe *dp
                = new kprobe_derived_probe (sess, finished_results, base,
                                            location, *it, 0, has_call,
                                            has_return, has_statement_num,
                                            has_maxactive, has_path,
                                            has_library, maxactive_val,
                                            path_tgt, library_tgt);
              finished_results.push_back (dp);
	    }
	}
    }
  else
    {
      // assert guru mode for absolute probes
      if ( has_statement_num && has_absolute && !base->privileged )
	throw SEMANTIC_ERROR (_("absolute statement probe in unprivileged script; need stap -g"), base->tok);

      finished_results.push_back (new kprobe_derived_probe (sess,
							    finished_results,
							    base,
							    location, "",
							    statement_num_val,
							    has_call,
							    has_return,
							    has_statement_num,
							    has_maxactive,
							    has_path,
							    has_library,
							    maxactive_val,
							    path_tgt,
							    library_tgt));
    }
}


void
kprobe_var_expanding_visitor::visit_entry_op (entry_op *e)
{
  expression *repl = e;

  if (has_return)
    {
      // expand the operand as if it weren't a return probe
      has_return = false;
      replace (e->operand);
      has_return = true;

      // XXX it would be nice to use gen_kretprobe_saved_return when
      // available, but it requires knowing the types already, which is
      // problematic for arbitrary expressons.
      repl = gen_mapped_saved_return (sess, e->operand, "entry",
				      add_block, add_block_tid,
				      add_call_probe, add_call_probe_tid);
    }
  provide (repl);
}


// ------------------------------------------------------------------------
//  Hardware breakpoint based probes.
// ------------------------------------------------------------------------

static const string TOK_HWBKPT("data");
static const string TOK_HWBKPT_WRITE("write");
static const string TOK_HWBKPT_RW("rw");
static const string TOK_LENGTH("length");

#define HWBKPT_READ 0
#define HWBKPT_WRITE 1
#define HWBKPT_RW 2
struct hwbkpt_derived_probe: public derived_probe
{
  hwbkpt_derived_probe (probe *base,
                        probe_point *location,
                        uint64_t addr,
			string symname,
			unsigned int len,
			bool has_only_read_access,
			bool has_only_write_access,
			bool has_rw_access
                        );
  Dwarf_Addr hwbkpt_addr;
  string symbol_name;
  unsigned int hwbkpt_access,hwbkpt_len;

  void printsig (std::ostream &o) const;
  void join_group (systemtap_session& s);
};

struct hwbkpt_derived_probe_group: public derived_probe_group
{
private:
  vector<hwbkpt_derived_probe*> hwbkpt_probes;

public:
  void enroll (hwbkpt_derived_probe* probe, systemtap_session& s);
  void emit_module_decls (systemtap_session& s);
  void emit_module_init (systemtap_session& s);
  void emit_module_exit (systemtap_session& s);
};

hwbkpt_derived_probe::hwbkpt_derived_probe (probe *base,
                                            probe_point *location,
                                            uint64_t addr,
                                            string symname,
                                            unsigned int len,
                                            bool has_only_read_access,
                                            bool has_only_write_access,
                                            bool):
  derived_probe (base, location, true /* .components soon rewritten */ ),
  hwbkpt_addr (addr),
  symbol_name (symname),
  hwbkpt_len (len)
{
  this->tok = base->tok;

  vector<probe_point::component*> comps;
  comps.push_back (new probe_point::component(TOK_KERNEL));

  if (hwbkpt_addr)
    comps.push_back (new probe_point::component (TOK_HWBKPT,
                                                 new literal_number(hwbkpt_addr, true)));
  else if (symbol_name.size())
    comps.push_back (new probe_point::component (TOK_HWBKPT, new literal_string(symbol_name)));

  comps.push_back (new probe_point::component (TOK_LENGTH, new literal_number(hwbkpt_len)));

  if (has_only_read_access)
    this->hwbkpt_access = HWBKPT_READ ;
//TODO add code for comps.push_back for read, since this flag is not for x86

  else
    {
      if (has_only_write_access)
        {
          this->hwbkpt_access = HWBKPT_WRITE ;
          comps.push_back (new probe_point::component(TOK_HWBKPT_WRITE));
        }
      else
        {
          this->hwbkpt_access = HWBKPT_RW ;
          comps.push_back (new probe_point::component(TOK_HWBKPT_RW));
        }
    }

  this->sole_location()->components = comps;
}

void hwbkpt_derived_probe::printsig (ostream& o) const
{
  sole_location()->print (o);
  printsig_nested (o);
}

void hwbkpt_derived_probe::join_group (systemtap_session& s)
{
  if (! s.hwbkpt_derived_probes)
    s.hwbkpt_derived_probes = new hwbkpt_derived_probe_group ();
  s.hwbkpt_derived_probes->enroll (this, s);
  this->group = s.hwbkpt_derived_probes;
}

void hwbkpt_derived_probe_group::enroll (hwbkpt_derived_probe* p, systemtap_session& s)
{
  hwbkpt_probes.push_back (p);

  unsigned max_hwbkpt_probes_by_arch = 0;
  if (s.architecture == "i386" || s.architecture == "x86_64")
    max_hwbkpt_probes_by_arch = 4;
  else if (s.architecture == "s390")
    max_hwbkpt_probes_by_arch = 1;

  if (hwbkpt_probes.size() >= max_hwbkpt_probes_by_arch)
    s.print_warning (_F("Too many hardware breakpoint probes requested for %s (%zu vs. %u)",
                          s.architecture.c_str(), hwbkpt_probes.size(), max_hwbkpt_probes_by_arch));
}

void
hwbkpt_derived_probe_group::emit_module_decls (systemtap_session& s)
{
  if (hwbkpt_probes.empty()) return;

  s.op->newline() << "/* ---- hwbkpt-based probes ---- */";

  s.op->newline() << "#include <linux/perf_event.h>";
  s.op->newline() << "#include <linux/hw_breakpoint.h>";
  s.op->newline();

  // Forward declare the master entry functions
  s.op->newline() << "#ifdef STAPCONF_PERF_HANDLER_NMI";
  s.op->newline() << "static int enter_hwbkpt_probe (struct perf_event *bp,";
  s.op->line() << " int nmi,";
  s.op->line() << " struct perf_sample_data *data,";
  s.op->line() << " struct pt_regs *regs);";
  s.op->newline() << "#else";
  s.op->newline() << "static int enter_hwbkpt_probe (struct perf_event *bp,";
  s.op->line() << " struct perf_sample_data *data,";
  s.op->line() << " struct pt_regs *regs);";
  s.op->newline() << "#endif";

  // Emit the actual probe list.

  s.op->newline() << "static struct perf_event_attr ";
  s.op->newline() << "stap_hwbkpt_probe_array[" << hwbkpt_probes.size() << "];";

  s.op->newline() << "static struct perf_event **";
  s.op->newline() << "stap_hwbkpt_ret_array[" << hwbkpt_probes.size() << "];";
  s.op->newline() << "static struct stap_hwbkpt_probe {";
  s.op->newline() << "int registered_p:1;";
// registered_p =  0 signifies a probe that is unregistered (or failed)
// registered_p =  1 signifies a probe that got registered successfully

  // Symbol Names are mostly small and uniform enough
  // to justify putting const char*.
  s.op->newline() << "const char * const symbol;";

  s.op->newline() << "const unsigned long address;";
  s.op->newline() << "uint8_t atype;";
  s.op->newline() << "unsigned int len;";
  s.op->newline() << "const struct stap_probe * const probe;";
  s.op->newline() << "} stap_hwbkpt_probes[] = {";
  s.op->indent(1);

  for (unsigned int it = 0; it < hwbkpt_probes.size(); it++)
    {
      hwbkpt_derived_probe* p = hwbkpt_probes.at(it);
      s.op->newline() << "{";
      if (p->symbol_name.size())
      s.op->line() << " .address=(unsigned long)0x0" << "ULL,";
      else
      s.op->line() << " .address=(unsigned long)0x" << hex << p->hwbkpt_addr << dec << "ULL,";
      switch(p->hwbkpt_access){
      case HWBKPT_READ:
		s.op->line() << " .atype=HW_BREAKPOINT_R ,";
		break;
      case HWBKPT_WRITE:
		s.op->line() << " .atype=HW_BREAKPOINT_W ,";
		break;
      case HWBKPT_RW:
		s.op->line() << " .atype=HW_BREAKPOINT_R|HW_BREAKPOINT_W ,";
		break;
	};
      s.op->line() << " .len=" << p->hwbkpt_len << ",";
      s.op->line() << " .probe=" << common_probe_init (p) << ",";
      s.op->line() << " .symbol=\"" << p->symbol_name << "\",";
      s.op->line() << " },";
    }
  s.op->newline(-1) << "};";

  // Emit the hwbkpt callback function
  s.op->newline() ;
  s.op->newline() << "#ifdef STAPCONF_PERF_HANDLER_NMI";
  s.op->newline() << "static int enter_hwbkpt_probe (struct perf_event *bp,";
  s.op->line() << " int nmi,";
  s.op->line() << " struct perf_sample_data *data,";
  s.op->line() << " struct pt_regs *regs) {";
  s.op->newline() << "#else";
  s.op->newline() << "static int enter_hwbkpt_probe (struct perf_event *bp,";
  s.op->line() << " struct perf_sample_data *data,";
  s.op->line() << " struct pt_regs *regs) {";
  s.op->newline() << "#endif";
  s.op->newline(1) << "unsigned int i;";
  s.op->newline() << "if (bp->attr.type != PERF_TYPE_BREAKPOINT) return -1;";
  s.op->newline() << "for (i=0; i<" << hwbkpt_probes.size() << "; i++) {";
  s.op->newline(1) << "struct perf_event_attr *hp = & stap_hwbkpt_probe_array[i];";
  // XXX: why not match stap_hwbkpt_ret_array[i] against bp instead?
  s.op->newline() << "if (bp->attr.bp_addr==hp->bp_addr && bp->attr.bp_type==hp->bp_type && bp->attr.bp_len==hp->bp_len) {";
  s.op->newline(1) << "struct stap_hwbkpt_probe *sdp = &stap_hwbkpt_probes[i];";
  common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "sdp->probe",
				 "stp_probe_type_hwbkpt");
  s.op->newline() << "if (user_mode(regs)) {";
  s.op->newline(1)<< "c->user_mode_p = 1;";
  s.op->newline() << "c->uregs = regs;";
  s.op->newline(-1) << "} else {";
  s.op->newline(1) << "c->kregs = regs;";
  s.op->newline(-1) << "}";
  s.op->newline() << "(*sdp->probe->ph) (c);";
  common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";
  s.op->newline() << "return 0;";
  s.op->newline(-1) << "}";
}

void
hwbkpt_derived_probe_group::emit_module_init (systemtap_session& s)
{
  s.op->newline() << "for (i=0; i<" << hwbkpt_probes.size() << "; i++) {";
  s.op->newline(1) << "struct stap_hwbkpt_probe *sdp = & stap_hwbkpt_probes[i];";
  s.op->newline() << "struct perf_event_attr *hp = & stap_hwbkpt_probe_array[i];";
  s.op->newline() << "void *addr = (void *) sdp->address;";
  s.op->newline() << "const char *hwbkpt_symbol_name = addr ? NULL : sdp->symbol;";
  s.op->newline() << "hw_breakpoint_init(hp);";
  s.op->newline() << "if (addr)";
  s.op->newline(1) << "hp->bp_addr = (unsigned long) addr;";
  s.op->newline(-1) << "else { ";
  s.op->newline(1) << "hp->bp_addr = kallsyms_lookup_name(hwbkpt_symbol_name);";
  s.op->newline() << "if (!hp->bp_addr) { ";
  s.op->newline(1) << "_stp_warn(\"Probe %s registration skipped: invalid symbol %s \",sdp->probe->pp,hwbkpt_symbol_name);";
  s.op->newline() << "continue;";
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";
  s.op->newline() << "hp->bp_type = sdp->atype;";

  // On x86 & x86-64, hp->bp_len is not just a number but a macro/enum (!?!).
  if (s.architecture == "i386" || s.architecture == "x86_64" )
    {
      s.op->newline() << "switch(sdp->len) {";
      s.op->newline() << "case 1:";
      s.op->newline(1) << "hp->bp_len = HW_BREAKPOINT_LEN_1;";
      s.op->newline() << "break;";
      s.op->newline(-1) << "case 2:";
      s.op->newline(1) << "hp->bp_len = HW_BREAKPOINT_LEN_2;";
      s.op->newline() << "break;";
      s.op->newline(-1) << "case 3:";
      s.op->newline() << "case 4:";
      s.op->newline(1) << "hp->bp_len = HW_BREAKPOINT_LEN_4;";
      s.op->newline() << "break;";
      s.op->newline(-1) << "case 5:";
      s.op->newline() << "case 6:";
      s.op->newline() << "case 7:";
      s.op->newline() << "case 8:";
      s.op->newline() << "default:"; // XXX: could instead reject
      s.op->newline(1) << "hp->bp_len = HW_BREAKPOINT_LEN_8;";
      s.op->newline() << "break;";
      s.op->newline(-1) << "}";
    }
  else // other architectures presumed straightforward
    s.op->newline() << "hp->bp_len = sdp->len;";

  s.op->newline() << "probe_point = sdp->probe->pp;"; // for error messages
  s.op->newline() << "#ifdef STAPCONF_HW_BREAKPOINT_CONTEXT";
  s.op->newline() << "stap_hwbkpt_ret_array[i] = register_wide_hw_breakpoint(hp, (void *)&enter_hwbkpt_probe, NULL);";
  s.op->newline() << "#else";
  s.op->newline() << "stap_hwbkpt_ret_array[i] = register_wide_hw_breakpoint(hp, (void *)&enter_hwbkpt_probe);";
  s.op->newline() << "#endif";
  s.op->newline() << "rc = 0;";
  s.op->newline() << "if (IS_ERR(stap_hwbkpt_ret_array[i])) {";
  s.op->newline(1) << "rc = PTR_ERR(stap_hwbkpt_ret_array[i]);";
  s.op->newline() << "stap_hwbkpt_ret_array[i] = 0;";
  s.op->newline(-1) << "}";
  s.op->newline() << "if (rc) {";
  s.op->newline(1) << "_stp_warn(\"Hwbkpt probe %s: registration error %d, addr %p, name %s\", probe_point, rc, addr, hwbkpt_symbol_name);";
  s.op->newline() << "sdp->registered_p = 0;";
  s.op->newline(-1) << "}";
  s.op->newline() << " else sdp->registered_p = 1;";
  s.op->newline(-1) << "}"; // for loop
}

void
hwbkpt_derived_probe_group::emit_module_exit (systemtap_session& s)
{
  //Unregister hwbkpt probes.
  s.op->newline() << "for (i=0; i<" << hwbkpt_probes.size() << "; i++) {";
  s.op->newline(1) << "struct stap_hwbkpt_probe *sdp = & stap_hwbkpt_probes[i];";
  s.op->newline() << "if (sdp->registered_p == 0) continue;";
  s.op->newline() << "unregister_wide_hw_breakpoint(stap_hwbkpt_ret_array[i]);";
  s.op->newline() << "sdp->registered_p = 0;";
  s.op->newline(-1) << "}";
}

struct hwbkpt_builder: public derived_probe_builder
{
  hwbkpt_builder() {}
  virtual void build(systemtap_session & sess,
		     probe * base,
		     probe_point * location,
		     literal_map_t const & parameters,
		     vector<derived_probe *> & finished_results);
};

void
hwbkpt_builder::build(systemtap_session & sess,
		      probe * base,
		      probe_point * location,
		      literal_map_t const & parameters,
		      vector<derived_probe *> & finished_results)
{
  string symbol_str_val;
  int64_t hwbkpt_address, len;
  bool has_addr, has_symbol_str, has_write, has_rw, has_len;

  if (! (sess.kernel_config["CONFIG_PERF_EVENTS"] == string("y")))
      throw SEMANTIC_ERROR (_("CONFIG_PERF_EVENTS not available on this kernel"),
                            location->components[0]->tok);
  if (! (sess.kernel_config["CONFIG_HAVE_HW_BREAKPOINT"] == string("y")))
      throw SEMANTIC_ERROR (_("CONFIG_HAVE_HW_BREAKPOINT not available on this kernel"),
                            location->components[0]->tok);

  has_addr = get_param (parameters, TOK_HWBKPT, hwbkpt_address);
  has_symbol_str = get_param (parameters, TOK_HWBKPT, symbol_str_val);
  has_len = get_param (parameters, TOK_LENGTH, len);
  has_write = (parameters.find(TOK_HWBKPT_WRITE) != parameters.end());
  has_rw = (parameters.find(TOK_HWBKPT_RW) != parameters.end());

  // Make an intermediate pp that is well-formed. It's pretty much the same as
  // the user-provided one, except that the addr literal is well-typed.
  probe_point* well_formed_loc = new probe_point(*location);
  well_formed_loc->well_formed = true;

  vector<probe_point::component*> well_formed_comps;
  vector<probe_point::component*>::iterator it;
  for (it = location->components.begin();
      it != location->components.end(); ++it)
    if ((*it)->functor == TOK_HWBKPT && has_addr)
      well_formed_comps.push_back(new probe_point::component(TOK_HWBKPT,
          new literal_number(hwbkpt_address, true /* hex */ )));
    else
      well_formed_comps.push_back(*it);
  well_formed_loc->components = well_formed_comps;
  probe *new_base = new probe (base, well_formed_loc);

  if (!has_len)
	len = 1;

  if (has_addr)
      finished_results.push_back (new hwbkpt_derived_probe (new_base,
							    location,
							    hwbkpt_address,
							    "",len,0,
							    has_write,
							    has_rw));
  else if (has_symbol_str)
      finished_results.push_back (new hwbkpt_derived_probe (new_base,
							    location,
							    0,
							    symbol_str_val,len,0,
							    has_write,
							    has_rw));
  else
    assert (0);
}

// ------------------------------------------------------------------------
// statically inserted kernel-tracepoint derived probes
// ------------------------------------------------------------------------

struct tracepoint_arg
{
  string name, c_type, typecast;
  bool usable, used, isptr;
  Dwarf_Die type_die;
  tracepoint_arg(): usable(false), used(false), isptr(false) {}
};

struct tracepoint_derived_probe: public derived_probe
{
  tracepoint_derived_probe (systemtap_session& s,
                            dwflpp& dw, Dwarf_Die& func_die,
                            const string& tracepoint_system,
                            const string& tracepoint_name,
                            probe* base_probe, probe_point* location);

  systemtap_session& sess;
  string tracepoint_system, tracepoint_name, header;
  vector <struct tracepoint_arg> args;

  void build_args(dwflpp& dw, Dwarf_Die& func_die);
  void getargs (std::list<std::string> &arg_set) const;
  void join_group (systemtap_session& s);
  void print_dupe_stamp(ostream& o);
};


struct tracepoint_derived_probe_group: public generic_dpg<tracepoint_derived_probe>
{
  void emit_module_decls (systemtap_session& s);
  void emit_module_init (systemtap_session& s);
  void emit_module_exit (systemtap_session& s);
};


struct tracepoint_var_expanding_visitor: public var_expanding_visitor
{
  tracepoint_var_expanding_visitor(dwflpp& dw, const string& probe_name,
                                   vector <struct tracepoint_arg>& args):
    dw (dw), probe_name (probe_name), args (args) {}
  dwflpp& dw;
  const string& probe_name;
  vector <struct tracepoint_arg>& args;

  void visit_target_symbol (target_symbol* e);
  void visit_target_symbol_arg (target_symbol* e);
  void visit_target_symbol_context (target_symbol* e);
};


void
tracepoint_var_expanding_visitor::visit_target_symbol_arg (target_symbol* e)
{
  string argname = e->sym_name();

  // search for a tracepoint parameter matching this name
  tracepoint_arg *arg = NULL;
  for (unsigned i = 0; i < args.size(); ++i)
    if (args[i].usable && args[i].name == argname)
      {
        arg = &args[i];
        arg->used = true;
        break;
      }

  if (arg == NULL)
    {
      set<string> vars;
      for (unsigned i = 0; i < args.size(); ++i)
        vars.insert("$" + args[i].name);
      vars.insert("$$name");
      vars.insert("$$parms");
      vars.insert("$$vars");
      string sugs = levenshtein_suggest(e->name, vars); // no need to limit, there's not that many

      // We hope that this value ends up not being referenced after all, so it
      // can be optimized out quietly.
      throw SEMANTIC_ERROR(_F("unable to find tracepoint variable '%s'%s",
                              e->name.c_str(), sugs.empty() ? "" :
                              (_(" (alternatives: ") + sugs + ")").c_str()), e->tok);
                              // NB: we use 'alternatives' because we list all
      // NB: we can have multiple errors, since a target variable
      // may be expanded in several different contexts:
      //     trace ("*") { $foo->bar }
    }

  // make sure we're not dereferencing base types or void
  bool deref_p = arg->isptr && !null_die(&arg->type_die);
  if (!deref_p)
    e->assert_no_components("tracepoint", true);

  // we can only write to dereferenced fields, and only if guru mode is on
  bool lvalue = is_active_lvalue(e);
  if (lvalue && (!dw.sess.guru_mode || e->components.empty()))
    throw SEMANTIC_ERROR(_F("write to tracepoint variable '%s' not permitted; need stap -g", e->name.c_str()), e->tok);

  // XXX: if a struct/union arg is passed by value, then writing to its fields
  // is also meaningless until you dereference past a pointer member.  It's
  // harder to detect and prevent that though...

  if (e->components.empty())
    {
      if (e->addressof)
        throw SEMANTIC_ERROR(_("cannot take address of tracepoint variable"), e->tok);

      // Just grab the value from the probe locals
      symbol* sym = new symbol;
      sym->tok = e->tok;
      sym->name = "__tracepoint_arg_" + arg->name;
      sym->type_details.reset(new exp_type_dwarf(&dw, &arg->type_die, false, false));
      provide (sym);
    }
  else
    {
      // make a copy of the original as a bare target symbol for the tracepoint
      // value, which will be passed into the dwarf dereferencing code
      target_symbol* e2 = deep_copy_visitor::deep_copy(e);
      e2->components.clear();

      if (e->check_pretty_print (lvalue))
        {
          dwarf_pretty_print dpp(dw, &arg->type_die, e2, deref_p, false, *e);
          dpp.expand()->visit (this);
          return;
        }

      bool userspace_p = false;
      string fname = (string(lvalue ? "_tracepoint_tvar_set" : "_tracepoint_tvar_get")
                      + "_" + e->sym_name()
                      + "_" + lex_cast(tick++));

      Dwarf_Die endtype;
      string code = dw.literal_stmt_for_pointer (&arg->type_die, e, lvalue, &endtype);

      functioncall* n = synthetic_embedded_deref_call(dw, &endtype, fname, code,
                                                      userspace_p, lvalue, e, e2);

      if (lvalue)
	provide_lvalue_call (n);

      // Revisit the functioncall so arguments can be expanded.
      n->visit (this);
    }
}


void
tracepoint_var_expanding_visitor::visit_target_symbol_context (target_symbol* e)
{
  if (e->addressof)
    throw SEMANTIC_ERROR(_("cannot take address of context variable"), e->tok);

  if (is_active_lvalue (e))
    throw SEMANTIC_ERROR(_F("write to tracepoint '%s' not permitted", e->name.c_str()), e->tok);

  if (e->name == "$$name" || e->name == "$$system")
    {
      e->assert_no_components("tracepoint");

      string member = (e->name == "$$name") ? "c->ips.tp.tracepoint_name"
                                            : "c->ips.tp.tracepoint_system";

      // Synthesize an embedded expression.
      embedded_expr *expr = new embedded_expr;
      expr->tok = e->tok;
      expr->code = string("/* string */ /* pure */ " +
                          member + " ? " + member + " : \"\"");
      provide (expr);
    }
  else if (e->name == "$$vars" || e->name == "$$parms")
    {
      e->assert_no_components("tracepoint", true);

      print_format* pf = print_format::create(e->tok, "sprintf");

      for (unsigned i = 0; i < args.size(); ++i)
        {
          if (!args[i].usable)
            continue;
          if (i > 0)
            pf->raw_components += " ";
          pf->raw_components += args[i].name;
          target_symbol *tsym = new target_symbol;
          tsym->tok = e->tok;
          tsym->name = "$" + args[i].name;
          tsym->components = e->components;

          // every variable should always be accessible!
          tsym->saved_conversion_error = 0;
          expression *texp = require<expression> (tsym); // NB: throws nothing ...
          if (tsym->saved_conversion_error) // ... but this is how we know it happened.
            {
              if (dw.sess.verbose>2)
                for (const semantic_error *c = tsym->saved_conversion_error;
                     c != 0; c = c->get_chain())
                  clog << _("variable location problem [man error::dwarf]: ") << c->what() << endl;
              pf->raw_components += "=?";
              continue;
            }

          if (e->check_pretty_print ())
            pf->raw_components += "=%s";
          else
            pf->raw_components += args[i].isptr ? "=%p" : "=%#x";
          pf->args.push_back(texp);
        }

      pf->components = print_format::string_to_components(pf->raw_components);
      provide (pf);
    }
  else
    assert(0); // shouldn't get here
}

void
tracepoint_var_expanding_visitor::visit_target_symbol (target_symbol* e)
{
  try
    {
      assert(e->name.size() > 0 && e->name[0] == '$');

      if (e->name == "$$name" || e->name == "$$system"
          || e->name == "$$parms" || e->name == "$$vars")
        visit_target_symbol_context (e);
      else
        visit_target_symbol_arg (e);
    }
  catch (const semantic_error &er)
    {
      e->chain (er);
      provide (e);
    }
}


tracepoint_derived_probe::tracepoint_derived_probe (systemtap_session& s,
                                                    dwflpp& dw, Dwarf_Die& func_die,
                                                    const string& tracepoint_system,
                                                    const string& tracepoint_name,
                                                    probe* base, probe_point* loc):
  derived_probe (base, loc, true /* .components soon rewritten */), sess (s),
  tracepoint_system (tracepoint_system), tracepoint_name (tracepoint_name)
{
  // create synthetic probe point name; preserve condition
  vector<probe_point::component*> comps;
  comps.push_back (new probe_point::component (TOK_KERNEL));

  // tag on system to the final name unless we're in compatibility mode so that
  // e.g. pn() returns just the name as before
  string final_name = tracepoint_name;
  if (!tracepoint_system.empty()
      && strverscmp(s.compatible.c_str(), "2.6") > 0)
    final_name = tracepoint_system + ":" + final_name;

  comps.push_back (new probe_point::component (TOK_TRACE,
                                               new literal_string(final_name)));
  this->sole_location()->components = comps;

  // fill out the available arguments in this tracepoint
  build_args(dw, func_die);

  // determine which header defined this tracepoint
  string decl_file = dwarf_decl_file(&func_die);
  header = decl_file;

#if 0 /* This convention is not enforced. */
  size_t header_pos = decl_file.rfind("trace/");
  if (header_pos == string::npos)
    throw SEMANTIC_ERROR ("cannot parse header location for tracepoint '"
                                  + tracepoint_name + "' in '"
                                  + decl_file + "'");
  header = decl_file.substr(header_pos);
#endif

  // tracepoints from FOO_event_types.h should really be included from FOO.h
  // XXX can dwarf tell us the include hierarchy?  it would be better to
  // ... walk up to see which one was directly included by tracequery.c
  // XXX: see also PR9993.
  size_t header_pos = header.find("_event_types");
  if (header_pos != string::npos)
    header.erase(header_pos, 12);

  // Now expand the local variables in the probe body
  tracepoint_var_expanding_visitor v (dw, name, args);
  v.replace (this->body);
  for (unsigned i = 0; i < args.size(); i++)
    if (args[i].used)
      {
	vardecl* v = new vardecl;
	v->name = "__tracepoint_arg_" + args[i].name;
	v->tok = this->tok;
	v->set_arity(0, this->tok);
	v->type = pe_long;
	v->synthetic = true;
	this->locals.push_back (v);
      }

  if (sess.verbose > 2)
    clog << "tracepoint-based " << name << " tracepoint='" << tracepoint_name << "'" << endl;
}


static bool
resolve_pointer_type(Dwarf_Die& die, bool& isptr)
{
  if (null_die(&die))
    {
      isptr = true;
      return true;
    }

  Dwarf_Die type;
  switch (dwarf_tag(&die))
    {
    case DW_TAG_typedef:
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
      // iterate on the referent type
      return (dwarf_attr_die(&die, DW_AT_type, &die)
              && resolve_pointer_type(die, isptr));

    case DW_TAG_base_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_structure_type:
    case DW_TAG_union_type:
      // base types will simply be treated as script longs
      // structs/unions must be referenced by pointer elsewhere
      isptr = false;
      return true;

    case DW_TAG_array_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      // pointer-like types can be treated as script longs,
      // and if we know their type, they can also be dereferenced
      isptr = true;
      type = die;
      while (dwarf_attr_die(&type, DW_AT_type, &type))
        {
          // It still might be a non-type, e.g. const void,
          // so we need to strip away all qualifiers.
          int tag = dwarf_tag(&type);
          if (tag != DW_TAG_typedef &&
              tag != DW_TAG_const_type &&
              tag != DW_TAG_volatile_type &&
              tag != DW_TAG_restrict_type)
            {
              die = type;
              return true;
            }
        }
      // otherwise use a null_die to indicate void
      std::memset(&die, 0, sizeof(die));
      return true;

    default:
      // should we consider other types too?
      return false;
    }
}


static bool
resolve_tracepoint_arg_type(tracepoint_arg& arg)
{
  if (!resolve_pointer_type(arg.type_die, arg.isptr))
    return false;

  if (arg.isptr)
    arg.typecast = "(intptr_t)";
  else if (dwarf_tag(&arg.type_die) == DW_TAG_structure_type ||
           dwarf_tag(&arg.type_die) == DW_TAG_union_type)
    {
      // for structs/unions which are passed by value, we turn it into
      // a pointer that can be dereferenced.
      arg.isptr = true;
      arg.typecast = "(intptr_t)&";
    }
  return true;
}


void
tracepoint_derived_probe::build_args(dwflpp&, Dwarf_Die& func_die)
{
  Dwarf_Die arg;
  if (dwarf_child(&func_die, &arg) == 0)
    do
      if (dwarf_tag(&arg) == DW_TAG_formal_parameter)
        {
          // build a tracepoint_arg for this parameter
          tracepoint_arg tparg;
          tparg.name = dwarf_diename(&arg) ?: "";

          // read the type of this parameter
          if (!dwarf_attr_die (&arg, DW_AT_type, &tparg.type_die)
              || !dwarf_type_name(&tparg.type_die, tparg.c_type))
            throw SEMANTIC_ERROR (_F("cannot get type of parameter '%s' of tracepoint '%s'",
                                     tparg.name.c_str(), tracepoint_name.c_str()));

          tparg.usable = resolve_tracepoint_arg_type(tparg);
          args.push_back(tparg);
          if (sess.verbose > 4)
            clog << _F("found parameter for tracepoint '%s': type:'%s' name:'%s' %s",
                       tracepoint_name.c_str(), tparg.c_type.c_str(), tparg.name.c_str(),
                       tparg.usable ? "ok" : "unavailable") << endl;
        }
    while (dwarf_siblingof(&arg, &arg) == 0);
}

void
tracepoint_derived_probe::getargs(std::list<std::string> &arg_set) const
{
  for (unsigned i = 0; i < args.size(); ++i)
    if (args[i].usable)
      arg_set.push_back("$"+args[i].name+":"+args[i].c_type);
}

void
tracepoint_derived_probe::join_group (systemtap_session& s)
{
  if (! s.tracepoint_derived_probes)
    s.tracepoint_derived_probes = new tracepoint_derived_probe_group ();
  s.tracepoint_derived_probes->enroll (this);
  this->group = s.tracepoint_derived_probes;
}


void
tracepoint_derived_probe::print_dupe_stamp(ostream& o)
{
  for (unsigned i = 0; i < args.size(); i++)
    if (args[i].used)
      o << "__tracepoint_arg_" << args[i].name << endl;
}


static vector<string> tracepoint_extra_decls (systemtap_session& s, const string& header)
{
  vector<string> they_live;
  // PR 9993
  // XXX: may need this to be configurable
  they_live.push_back ("#include <linux/skbuff.h>");

  // PR11649: conditional extra header
  // for kvm tracepoints in 2.6.33ish
  if (s.kernel_config["CONFIG_KVM"] != string("")) {
    they_live.push_back ("#include <linux/kvm_host.h>");
  }

  if (header.find("xfs") != string::npos && s.kernel_config["CONFIG_XFS_FS"] != string("")) {
    they_live.push_back ("#define XFS_BIG_BLKNOS 1");
    if (s.kernel_source_tree != "")
      they_live.push_back ("#include \"fs/xfs/xfs_types.h\""); // in kernel-source tree
    they_live.push_back ("struct xfs_mount;");
    they_live.push_back ("struct xfs_inode;");
    they_live.push_back ("struct xfs_buf;");
    they_live.push_back ("struct xfs_bmbt_irec;");
    they_live.push_back ("struct xfs_trans;");
  }

  if (header.find("nfs") != string::npos && s.kernel_config["CONFIG_NFSD"] != string("")) {
    they_live.push_back ("struct rpc_task;");
  }
  // RHEL6.3
  if (header.find("rpc") != string::npos && s.kernel_config["CONFIG_NFSD"] != string("")) {
    they_live.push_back ("struct rpc_clnt;");
    they_live.push_back ("struct rpc_wait_queue;");
  }

  they_live.push_back ("#include <asm/cputime.h>");

  // linux 3.0
  they_live.push_back ("struct cpu_workqueue_struct;");

  if (header.find("ext4") != string::npos && s.kernel_config["CONFIG_EXT4_FS"] != string(""))
    if (s.kernel_source_tree != "")
      they_live.push_back ("#include \"fs/ext4/ext4.h\""); // in kernel-source tree

  if (header.find("ext3") != string::npos)
    they_live.push_back ("struct ext3_reserve_window_node;");

  if (header.find("workqueue") != string::npos)
    {
      they_live.push_back ("struct pool_workqueue;");
      they_live.push_back ("struct work_struct;");
    }

  if (header.find("asoc") != string::npos)
    they_live.push_back ("struct snd_soc_dapm_path;");

  if (header.find("9p") != string::npos)
    {
      they_live.push_back ("struct p9_client;");
      they_live.push_back ("struct p9_fcall;");
    }

  if (header.find("bcache") != string::npos)
    {
      they_live.push_back ("struct bkey;");
      they_live.push_back ("struct btree;");
      they_live.push_back ("struct cache_set;");
      they_live.push_back ("struct cache;");
    }

  if (header.find("f2fs") != string::npos)
    {
      // cannot get fs/f2fs/f2fs.h #included
      they_live.push_back ("typedef u32 block_t;");
      they_live.push_back ("typedef u32 nid_t;");
    }

  if (header.find("radeon") != string::npos)
    they_live.push_back ("struct radeon_bo;");

  // argh, 3.11, i915_trace.h -> i915_drv.h -> i915_reg.h without -I
  // also brcms_trace_events.h -> ... -> "types.h"
  // XXX: need a way to add a temporary -I flag

  if (header.find("/ath/") != string::npos)
    they_live.push_back ("struct ath5k_hw;");


  return they_live;
}


void
tracepoint_derived_probe_group::emit_module_decls (systemtap_session& s)
{
  if (probes.empty())
    return;

  s.op->newline() << "/* ---- tracepoint probes ---- */";
  s.op->newline() << "#include <linux/stp_tracepoint.h>" << endl;
  s.op->newline();


  // We create a MODULE_aux_N.c file for each tracepoint header, to allow them
  // to be separately compiled.  That's because kernel tracepoint headers sometimes
  // conflict.  PR13155.

  map<string,translator_output*> per_header_aux;
  // GC NB: the translator_output* structs are owned/retained by the systemtap_session.

  for (unsigned i = 0; i < probes.size(); ++i)
    {
      tracepoint_derived_probe *p = probes[i];
      string header = p->header;

      // We cache the auxiliary output files on a per-header basis.  We don't
      // need one aux file per tracepoint, only one per tracepoint-header.
      translator_output *tpop = per_header_aux[header];
      if (tpop == 0)
        {
          tpop = s.op_create_auxiliary();
          per_header_aux[header] = tpop;

          // PR9993: Add extra headers to work around undeclared types in individual
          // include/trace/foo.h files
          const vector<string>& extra_decls = tracepoint_extra_decls (s, header);
          for (unsigned z=0; z<extra_decls.size(); z++)
            tpop->newline() << extra_decls[z] << "\n";

          // strip include/ substring, the same way as done in get_tracequery_module()
          size_t root_pos = header.rfind("include/");
          header = ((root_pos != string::npos) ? header.substr(root_pos + 8) : header);

          tpop->newline() << "#include <linux/stp_tracepoint.h>" << endl;
          tpop->newline() << "#include <" << header << ">";
        }

      // collect the args that are actually in use
      vector<const tracepoint_arg*> used_args;
      for (unsigned j = 0; j < p->args.size(); ++j)
        if (p->args[j].used)
          used_args.push_back(&p->args[j]);

      // forward-declare the generated-side tracepoint callback, and define the
      // generated-side tracepoint callback in the main translator-output
      string enter_real_fn = "enter_real_tracepoint_probe_" + lex_cast(i);
      if (used_args.empty())
        {
          tpop->newline() << "STP_TRACE_ENTER_REAL_NOARGS(" << enter_real_fn << ");";
          s.op->newline() << "STP_TRACE_ENTER_REAL_NOARGS(" << enter_real_fn << ")";
        }
      else
        {
          tpop->newline() << "STP_TRACE_ENTER_REAL(" << enter_real_fn;
          s.op->newline() << "STP_TRACE_ENTER_REAL(" << enter_real_fn;
          s.op->indent(2);
          for (unsigned j = 0; j < used_args.size(); ++j)
            {
              tpop->line() << ", int64_t";
              s.op->newline() << ", int64_t __tracepoint_arg_" << used_args[j]->name;
            }
          tpop->line() << ");";
          s.op->newline() << ")";
          s.op->indent(-2);
        }
      s.op->newline() << "{";
      s.op->newline(1) << "const struct stap_probe * const probe = "
                       << common_probe_init (p) << ";";
      common_probe_entryfn_prologue (s, "STAP_SESSION_RUNNING", "probe",
				     "stp_probe_type_tracepoint");
      s.op->newline() << "c->ips.tp.tracepoint_system = "
                      << lex_cast_qstring (p->tracepoint_system)
                      << ";";
      s.op->newline() << "c->ips.tp.tracepoint_name = "
                      << lex_cast_qstring (p->tracepoint_name)
                      << ";";
      for (unsigned j = 0; j < used_args.size(); ++j)
        {
          s.op->newline() << "c->probe_locals." << p->name
                          << "." + s.up->c_localname("__tracepoint_arg_" + used_args[j]->name)
                          << " = __tracepoint_arg_" << used_args[j]->name << ";";
        }
      s.op->newline() << "(*probe->ph) (c);";
      common_probe_entryfn_epilogue (s, true, otf_safe_context(s));
      s.op->newline(-1) << "}";

      // define the real tracepoint callback function
      string enter_fn = "enter_tracepoint_probe_" + lex_cast(i);
      if (p->args.empty())
        tpop->newline() << "static STP_TRACE_ENTER_NOARGS(" << enter_fn << ")";
      else
        {
          tpop->newline() << "static STP_TRACE_ENTER(" << enter_fn;
          s.op->indent(2);
          for (unsigned j = 0; j < p->args.size(); ++j)
            {
              tpop->newline() << ", " << p->args[j].c_type
                              << " __tracepoint_arg_" << p->args[j].name;
            }
          tpop->newline() << ")";
          s.op->indent(-2);
        }
      tpop->newline() << "{";
      tpop->newline(1) << enter_real_fn << "(";
      tpop->indent(2);
      for (unsigned j = 0; j < used_args.size(); ++j)
        {
          if (j > 0)
            tpop->line() << ", ";
          tpop->newline() << "(int64_t)" << used_args[j]->typecast
                          << "__tracepoint_arg_" << used_args[j]->name;
        }
      tpop->newline() << ");";
      tpop->newline(-3) << "}";


      // emit normalized registration functions
      tpop->newline() << "int register_tracepoint_probe_" << i << "(void) {";
      tpop->newline(1) << "return STP_TRACE_REGISTER(" << p->tracepoint_name
                       << ", " << enter_fn << ");";
      tpop->newline(-1) << "}";

      // NB: we're not prepared to deal with unreg failures.  However, failures
      // can only occur if the tracepoint doesn't exist (yet?), or if we
      // weren't even registered.  The former should be OKed by the initial
      // registration call, and the latter is safe to ignore.
      tpop->newline() << "void unregister_tracepoint_probe_" << i << "(void) {";
      tpop->newline(1) << "(void) STP_TRACE_UNREGISTER(" << p->tracepoint_name
                       << ", " << enter_fn << ");";
      tpop->newline(-1) << "}";
      tpop->newline();

      // declare normalized registration functions
      s.op->newline() << "int register_tracepoint_probe_" << i << "(void);";
      s.op->newline() << "void unregister_tracepoint_probe_" << i << "(void);";

      tpop->assert_0_indent();
    }

  // emit an array of registration functions for easy init/shutdown
  s.op->newline() << "static struct stap_tracepoint_probe {";
  s.op->newline(1) << "int (*reg)(void);";
  s.op->newline(0) << "void (*unreg)(void);";
  s.op->newline(-1) << "} stap_tracepoint_probes[] = {";
  s.op->indent(1);
  for (unsigned i = 0; i < probes.size(); ++i)
    {
      s.op->newline () << "{";
      s.op->line() << " .reg=&register_tracepoint_probe_" << i << ",";
      s.op->line() << " .unreg=&unregister_tracepoint_probe_" << i;
      s.op->line() << " },";
    }
  s.op->newline(-1) << "};";
  s.op->newline();
}


void
tracepoint_derived_probe_group::emit_module_init (systemtap_session &s)
{
  if (probes.size () == 0)
    return;

  s.op->newline() << "/* init tracepoint probes */";
  s.op->newline() << "for (i=0; i<" << probes.size() << "; i++) {";
  s.op->newline(1) << "rc = stap_tracepoint_probes[i].reg();";
  s.op->newline() << "if (rc) {";
  s.op->newline(1) << "for (j=i-1; j>=0; j--)"; // partial rollback
  s.op->newline(1) << "stap_tracepoint_probes[j].unreg();";
  s.op->newline(-1) << "break;"; // don't attempt to register any more probes
  s.op->newline(-1) << "}";
  s.op->newline(-1) << "}";

  // This would be technically proper (on those autoconf-detectable
  // kernels that include this function in tracepoint.h), however we
  // already make several calls to synchronze_sched() during our
  // shutdown processes.

  // s.op->newline() << "if (rc)";
  // s.op->newline(1) << "tracepoint_synchronize_unregister();";
  // s.op->indent(-1);
}


void
tracepoint_derived_probe_group::emit_module_exit (systemtap_session& s)
{
  if (probes.empty())
    return;

  s.op->newline() << "/* deregister tracepoint probes */";
  s.op->newline() << "for (i=0; i<" << probes.size() << "; i++)";
  s.op->newline(1) << "stap_tracepoint_probes[i].unreg();";
  s.op->indent(-1);

  // Not necessary: see above.

  // s.op->newline() << "tracepoint_synchronize_unregister();";
}


struct tracepoint_query : public base_query
{
  probe * base_probe;
  probe_point * base_loc;
  vector<derived_probe *> & results;
  set<string> probed_names;

  void handle_query_module();
  int handle_query_cu(Dwarf_Die * cudie);
  int handle_query_func(Dwarf_Die * func);
  void query_library (const char *) {}
  void query_plt (const char *entry, size_t addr) {}

  static int tracepoint_query_cu (Dwarf_Die * cudie, tracepoint_query * q);
  static int tracepoint_query_func (Dwarf_Die * func, tracepoint_query * q);

  tracepoint_query(dwflpp & dw, const string & tracepoint,
                   probe * base_probe, probe_point * base_loc,
                   vector<derived_probe *> & results):
    base_query(dw, "*"), base_probe(base_probe),
    base_loc(base_loc), results(results)
  {
    // The user may have specified the system to probe, e.g. all of the
    // following are possible:
    //
    //   sched_switch --> tracepoint named sched_switch
    //   sched:sched_switch --> tracepoint named sched_switch in the sched system
    //   sch*:sched_* --> system starts with sch and tracepoint starts with sched_
    //   sched:* --> all tracepoints in system sched
    //   *:sched_switch --> same as just sched_switch

    size_t sys_pos = tracepoint.find(':');
    if (sys_pos == string::npos)
      {
        this->system = "";
        this->tracepoint = tracepoint;
      }
    else
      {
        if (strverscmp(sess.compatible.c_str(), "2.6") <= 0)
          throw SEMANTIC_ERROR (_("SYSTEM:TRACEPOINT syntax not supported "
                                  "with --compatible <= 2.6"));

        this->system = tracepoint.substr(0, sys_pos);
        this->tracepoint = tracepoint.substr(sys_pos+1);
      }

    // make sure we have something to query (filters out e.g. "" and ":")
    if (this->tracepoint.empty())
      throw SEMANTIC_ERROR (_("invalid tracepoint string provided"));
  }

private:
  string system; // target subsystem(s) to query
  string tracepoint; // target tracepoint(s) to query
  string current_system; // subsystem of module currently being visited

  string retrieve_trace_system();
};

// name of section where TRACE_SYSTEM is stored
// (see tracepoint_builder::get_tracequery_modules())
#define STAP_TRACE_SYSTEM ".stap_trace_system"

string
tracepoint_query::retrieve_trace_system()
{
  Dwarf_Addr bias;
  Elf* elf = dwfl_module_getelf(dw.module, &bias);
  if (!elf)
    return "";

  size_t shstrndx;
  if (elf_getshdrstrndx(elf, &shstrndx))
    return "";

  Elf_Scn *scn = NULL;
  GElf_Shdr shdr_mem;

  while ((scn = elf_nextscn(elf, scn)))
    {
      if (gelf_getshdr(scn, &shdr_mem) == NULL)
        return "";

      const char *name = elf_strptr(elf, shstrndx, shdr_mem.sh_name);
      if (name == NULL)
        return "";

      if (strcmp(name, STAP_TRACE_SYSTEM) == 0)
        break;
    }

  if (scn == NULL)
    return "";

  // Extract saved TRACE_SYSTEM in section
  Elf_Data *data = elf_getdata(scn, NULL);
  if (!data)
    return "";

  return string((char*)data->d_buf);
}


void
tracepoint_query::handle_query_module()
{
  // Get the TRACE_SYSTEM for this module, if any. It will be found in the
  // STAP_TRACE_SYSTEM section if it exists.
  current_system = retrieve_trace_system();

  // check if user wants a specific system
  if (!system.empty())
    {
      // don't need to go further if module has no system or doesn't
      // match the one we want
      if (current_system.empty()
          || !dw.function_name_matches_pattern(current_system, system))
        return;
    }

  // look for the tracepoints in each CU
  dw.iterate_over_cus(tracepoint_query_cu, this, false);
}


int
tracepoint_query::handle_query_cu(Dwarf_Die * cudie)
{
  dw.focus_on_cu (cudie);
  dw.mod_info->get_symtab();

  // look at each function to see if it's a tracepoint
  string function = "stapprobe_" + tracepoint;
  return dw.iterate_over_functions (tracepoint_query_func, this, function);
}


int
tracepoint_query::handle_query_func(Dwarf_Die * func)
{
  dw.focus_on_function (func);

  assert(startswith(dw.function_name, "stapprobe_"));
  string tracepoint_instance = dw.function_name.substr(10);

  // check for duplicates -- sometimes tracepoint headers may be indirectly
  // included in more than one of our tracequery modules.
  if (!probed_names.insert(tracepoint_instance).second)
    return DWARF_CB_OK;

  // PR17126: blacklist
  if (!sess.guru_mode)
    {
      if ((sess.architecture.substr(0,3) == "ppc" ||
           sess.architecture.substr(0,7) == "powerpc") &&
          (tracepoint_instance == "hcall_entry" ||
           tracepoint_instance == "hcall_exit"))
        {
          sess.print_warning(_F("tracepoint %s is blacklisted on architecture %s",
                                tracepoint_instance.c_str(), sess.architecture.c_str()));
          return DWARF_CB_OK;
        }
  }

  derived_probe *dp = new tracepoint_derived_probe (dw.sess, dw, *func,
                                                    current_system,
                                                    tracepoint_instance,
                                                    base_probe, base_loc);
  results.push_back (dp);
  return DWARF_CB_OK;
}


int
tracepoint_query::tracepoint_query_cu (Dwarf_Die * cudie, tracepoint_query * q)
{
  if (pending_interrupts) return DWARF_CB_ABORT;
  return q->handle_query_cu(cudie);
}


int
tracepoint_query::tracepoint_query_func (Dwarf_Die * func, tracepoint_query * q)
{
  if (pending_interrupts) return DWARF_CB_ABORT;
  return q->handle_query_func(func);
}


struct tracepoint_builder: public derived_probe_builder
{
private:
  dwflpp *dw;
  bool init_dw(systemtap_session& s);
  void get_tracequery_modules(systemtap_session& s,
                              const vector<string>& headers,
                              vector<string>& modules);

public:

  tracepoint_builder(): dw(0) {}
  ~tracepoint_builder() { delete dw; }

  void build_no_more (systemtap_session& s)
  {
    if (dw && s.verbose > 3)
      clog << _("tracepoint_builder releasing dwflpp") << endl;
    delete dw;
    dw = NULL;

    delete_session_module_cache (s);
  }

  void build(systemtap_session& s,
             probe *base, probe_point *location,
             literal_map_t const& parameters,
             vector<derived_probe*>& finished_results);
};



// Create (or cache) one or more tracequery .o modules, based upon the
// tracepoint-related header files given.  Return the generated or cached
// modules[].

void
tracepoint_builder::get_tracequery_modules(systemtap_session& s,
                                           const vector<string>& headers,
                                           vector<string>& modules)
{
  if (s.verbose > 2)
    {
      clog << _F("Pass 2: getting a tracepoint query for %zu headers: ", headers.size()) << endl;
      for (size_t i = 0; i < headers.size(); ++i)
        clog << "  " << headers[i] << endl;
    }

  map<string,string> headers_cache_obj;  // header name -> cache/.../tracequery_hash.o file name
  // Map the headers to cache .o names.  Note that this has side-effects of
  // creating the $SYSTEMTAP_DIR/.cache/XX/... directory and the hash-log file,
  // so we prefer not to repeat this.
  vector<string> uncached_headers;
  for (size_t i=0; i<headers.size(); i++)
    headers_cache_obj[headers[i]] = find_tracequery_hash(s, headers[i]);

  // They may be in the cache already.
  if (s.use_cache && !s.poison_cache)
    for (size_t i=0; i<headers.size(); i++)
      {
        // see if the cached module exists
        const string& tracequery_path = headers_cache_obj[headers[i]];
        if (!tracequery_path.empty() && file_exists(tracequery_path))
          {
            if (s.verbose > 2)
              clog << _F("Pass 2: using cached %s", tracequery_path.c_str()) << endl;

            // an empty file is a cached failure
            if (get_file_size(tracequery_path) > 0)
              modules.push_back (tracequery_path);
          }
        else
          uncached_headers.push_back(headers[i]);
      }
  else
    uncached_headers = headers;

  // If we have nothing left to search for, quit
  if (uncached_headers.empty()) return;

  map<string,string> headers_tracequery_src; // header -> C-source code mapping

  // We could query several subsets of headers[] to make this go
  // faster, but let's KISS and do one at a time.
  for (size_t i=0; i<uncached_headers.size(); i++)
    {
      const string& header = uncached_headers[i];

      // create a tracequery source file
      ostringstream osrc;

      // PR9993: Add extra headers to work around undeclared types in individual
      // include/trace/foo.h files
      vector<string> short_decls = tracepoint_extra_decls(s, header);

      // add each requested tracepoint header
      size_t root_pos = header.rfind("include/");
      short_decls.push_back(string("#include <") +
                            ((root_pos != string::npos) ? header.substr(root_pos + 8) : header) +
                            string(">"));

      osrc << "#ifdef CONFIG_TRACEPOINTS" << endl;
      osrc << "#include <linux/tracepoint.h>" << endl;

      // the kernel has changed this naming a few times, previously TPPROTO,
      // TP_PROTO, TPARGS, TP_ARGS, etc.  so let's just dupe the latest.
      osrc << "#ifndef PARAMS" << endl;
      osrc << "#define PARAMS(args...) args" << endl;
      osrc << "#endif" << endl;

      // override DECLARE_TRACE to synthesize probe functions for us
      osrc << "#undef DECLARE_TRACE" << endl;
      osrc << "#define DECLARE_TRACE(name, proto, args) \\" << endl;
      osrc << "  void stapprobe_##name(proto) {}" << endl;

      // 2.6.35 added the NOARGS variant, but it's the same for us
      osrc << "#undef DECLARE_TRACE_NOARGS" << endl;
      osrc << "#define DECLARE_TRACE_NOARGS(name) \\" << endl;
      osrc << "  DECLARE_TRACE(name, void, )" << endl;

      // 2.6.38 added the CONDITION variant, which can also just redirect
      osrc << "#undef DECLARE_TRACE_CONDITION" << endl;
      osrc << "#define DECLARE_TRACE_CONDITION(name, proto, args, cond) \\" << endl;
      osrc << "  DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))" << endl;

      // older tracepoints used DEFINE_TRACE, so redirect that too
      osrc << "#undef DEFINE_TRACE" << endl;
      osrc << "#define DEFINE_TRACE(name, proto, args) \\" << endl;
      osrc << "  DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))" << endl;

      // add the specified decls/#includes
      for (unsigned z=0; z<short_decls.size(); z++)
        osrc << "#undef TRACE_INCLUDE_FILE\n"
             << "#undef TRACE_INCLUDE_PATH\n"
             << short_decls[z] << "\n";

      // create a section that will hold the TRACE_SYSTEM for this header
      osrc << "#ifdef TRACE_SYSTEM" << endl;
      osrc << "const char stap_trace_system[]" << endl;
      osrc << "  __attribute__((section(\"" STAP_TRACE_SYSTEM "\")))" << endl;
      osrc << "    = __stringify(TRACE_SYSTEM);" << endl;
      osrc << "#endif" << endl;

      // finish up the module source
      osrc << "#endif /* CONFIG_TRACEPOINTS */" << endl;

      // save the source file away
      headers_tracequery_src[header] = osrc.str();
    }

  // now build them all together
  map<string,string> tracequery_objs = make_tracequeries(s, headers_tracequery_src);

  // now extend the modules list, and maybe plop them into the cache
  for (size_t i=0; i<uncached_headers.size(); i++)
    {
      const string& header = uncached_headers[i];
      const string& tracequery_obj = tracequery_objs[header];
      const string& tracequery_path = headers_cache_obj[header];
      if (tracequery_obj !="" && file_exists(tracequery_obj))
        {
          modules.push_back (tracequery_obj);
          if (s.use_cache)
            copy_file(tracequery_obj, tracequery_path, s.verbose > 2);
        }
      else if (s.use_cache)
        // cache an empty file for failures
        copy_file("/dev/null", tracequery_path, s.verbose > 2);
    }
}



bool
tracepoint_builder::init_dw(systemtap_session& s)
{
  if (dw != NULL)
    return true;

  vector<string> tracequery_modules;
  vector<string> system_headers;

  glob_t trace_glob;

  // find kernel_source_tree from DW_AT_comp_dir
  if (s.kernel_source_tree == "")
    {
      unsigned found;
      Dwfl *dwfl = setup_dwfl_kernel ("kernel", &found, s);
      if (found)
        {
          Dwarf_Die *cudie = 0;
          Dwarf_Addr bias;
          while ((cudie = dwfl_nextcu (dwfl, cudie, &bias)) != NULL)
            {
              assert_no_interrupts();
              Dwarf_Attribute attr;
              const char* name = dwarf_formstring (dwarf_attr (cudie, DW_AT_comp_dir, &attr));
              if (name)
                {
                  // Before we try to use it, check that the path actually
                  // exists locally and is distinct from the build tree.
                  if (!file_exists(name))
                    {
                      if (s.verbose > 2)
                        clog << _F("Ignoring inaccessible kernel source tree (DW_AT_comp_dir) at '%s'", name) << endl;
                    }
                  else if (resolve_path(name) == resolve_path(s.kernel_build_tree))
                    {
                      if (s.verbose > 2)
                        clog << _F("Ignoring duplicate kernel source tree (DW_AT_comp_dir) at '%s'", name) << endl;
                    }
                  else
                    {
                      if (s.verbose > 2)
                        clog << _F("Located kernel source tree (DW_AT_comp_dir) at '%s'", name) << endl;
                      s.kernel_source_tree = name;
                    }

                  break; // skip others; modern Kbuild uses same comp_dir for them all
                }
            }
        }
      dwfl_end (dwfl);
    }

  // find kernel_source_tree from a source link, when different from build
  if (s.kernel_source_tree == "" && endswith(s.kernel_build_tree, "/build"))
    {
      string source_tree = s.kernel_build_tree;
      source_tree.replace(source_tree.length() - 5, 5, "source");
      if (file_exists(source_tree) &&
          resolve_path(source_tree) != resolve_path(s.kernel_build_tree))
        {
          if (s.verbose > 2)
            clog << _F("Located kernel source tree at '%s'", source_tree.c_str()) << endl;
          s.kernel_source_tree = source_tree;
        }
    }

  // prefixes
  vector<string> glob_prefixes;
  glob_prefixes.push_back (s.kernel_build_tree);
  if (s.kernel_source_tree != "")
    glob_prefixes.push_back (s.kernel_source_tree);

  // suffixes
  vector<string> glob_suffixes;
  glob_suffixes.push_back("include/trace/events/*.h");
  glob_suffixes.push_back("include/trace/*.h");
  glob_suffixes.push_back("include/ras/*_event.h");
  glob_suffixes.push_back("arch/x86/kvm/*trace.h");
  glob_suffixes.push_back("arch/x86/kernel/*trace.h");
  glob_suffixes.push_back("arch/*/include/asm/trace*.h");
  glob_suffixes.push_back("arch/*/include/asm/trace/*.h");
  glob_suffixes.push_back("fs/xfs/linux-*/xfs_tr*.h");
  glob_suffixes.push_back("fs/*/*trace*.h");
  glob_suffixes.push_back("net/*/*trace*.h");
  glob_suffixes.push_back("sound/pci/hda/*_trace.h");
  glob_suffixes.push_back("drivers/gpu/drm/*_trace.h");
  glob_suffixes.push_back("drivers/gpu/drm/*/*_trace.h");
  glob_suffixes.push_back("drivers/net/wireless/*/*/*trace*.h");
  glob_suffixes.push_back("drivers/usb/host/*trace*.h");

  // see also tracepoint_extra_decls above

  // compute cartesian product
  vector<string> globs;
  for (unsigned i=0; i<glob_prefixes.size(); i++)
    for (unsigned j=0; j<glob_suffixes.size(); j++)
      globs.push_back (glob_prefixes[i]+string("/")+glob_suffixes[j]);

  set<string> duped_headers;
  for (unsigned z = 0; z < globs.size(); z++)
    {
      string glob_str = globs[z];
      if (s.verbose > 3)
        clog << _("Checking tracepoint glob ") << glob_str << endl;

      int r = glob(glob_str.c_str(), 0, NULL, &trace_glob);
      if (r == GLOB_NOSPACE || r == GLOB_ABORTED)
        throw runtime_error("Error globbing tracepoint");

      for (unsigned i = 0; i < trace_glob.gl_pathc; ++i)
        {
          string header(trace_glob.gl_pathv[i]);

          // filter out a few known "internal-only" headers
          if (endswith(header, "/define_trace.h") ||
              endswith(header, "/ftrace.h")       ||
              endswith(header, "/trace_events.h") ||
              endswith(header, "_event_types.h"))
            continue;

          // With headers now plopped under arch/FOO/include/asm/*,
          // the following logic miss some tracepoints.
#if 0
          // skip identical headers from the build and source trees.
          size_t root_pos = header.rfind("include/");
          if (root_pos != string::npos &&
              !duped_headers.insert(header.substr(root_pos + 8)).second)
            continue;
#endif

          system_headers.push_back(header);
        }
      globfree(&trace_glob);
    }

  // Build tracequery modules
  get_tracequery_modules(s, system_headers, tracequery_modules);

  // TODO: consider other sources of tracepoint headers too, like from
  // a command-line parameter or some environment or .systemtaprc

  dw = new dwflpp(s, tracequery_modules, true);
  return true;
}

void
tracepoint_builder::build(systemtap_session& s,
                          probe *base, probe_point *location,
                          literal_map_t const& parameters,
                          vector<derived_probe*>& finished_results)
{
  if (!init_dw(s))
    return;

  string tracepoint;
  assert(get_param (parameters, TOK_TRACE, tracepoint));

  tracepoint_query q(*dw, tracepoint, base, location, finished_results);
  unsigned results_pre = finished_results.size();
  dw->iterate_over_modules<base_query>(&query_module, &q);
  unsigned results_post = finished_results.size();

  // Did we fail to find a match? Let's suggest something!
  if (results_pre == results_post)
    {
      size_t pos;
      string sugs = suggest_dwarf_functions(s, q.visited_modules, tracepoint);
      while ((pos = sugs.find("stapprobe_")) != string::npos)
        sugs.erase(pos, string("stapprobe_").size());
      if (!sugs.empty())
        throw SEMANTIC_ERROR (_NF("no match (similar tracepoint: %s)",
                                  "no match (similar tracepoints: %s)",
                                  sugs.find(',') == string::npos,
                                  sugs.c_str()));
    }
}


// ------------------------------------------------------------------------
//  Standard tapset registry.
// ------------------------------------------------------------------------

void
register_standard_tapsets(systemtap_session & s)
{
  register_tapset_been(s);
  register_tapset_itrace(s);
  register_tapset_mark(s);
  register_tapset_procfs(s);
  register_tapset_timers(s);
  register_tapset_netfilter(s);
  register_tapset_utrace(s);

  // dwarf-based kprobe/uprobe parts
  dwarf_derived_probe::register_patterns(s);

  // XXX: user-space starter set
  s.pattern_root->bind_num(TOK_PROCESS)
    ->bind_num(TOK_STATEMENT)->bind(TOK_ABSOLUTE)
    ->bind_privilege(pr_all)
    ->bind(new uprobe_builder ());
  s.pattern_root->bind_num(TOK_PROCESS)
    ->bind_num(TOK_STATEMENT)->bind(TOK_ABSOLUTE)->bind(TOK_RETURN)
    ->bind_privilege(pr_all)
    ->bind(new uprobe_builder ());

  // kernel tracepoint probes
  s.pattern_root->bind(TOK_KERNEL)->bind_str(TOK_TRACE)
    ->bind(new tracepoint_builder());

  // Kprobe based probe
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_FUNCTION)
     ->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_FUNCTION)->bind(TOK_CALL)
     ->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_MODULE)
     ->bind_str(TOK_FUNCTION)->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_MODULE)
     ->bind_str(TOK_FUNCTION)->bind(TOK_CALL)->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_FUNCTION)->bind(TOK_RETURN)
     ->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_FUNCTION)->bind(TOK_RETURN)
     ->bind_num(TOK_MAXACTIVE)->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_MODULE)
     ->bind_str(TOK_FUNCTION)->bind(TOK_RETURN)->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_str(TOK_MODULE)
     ->bind_str(TOK_FUNCTION)->bind(TOK_RETURN)
     ->bind_num(TOK_MAXACTIVE)->bind(new kprobe_builder());
  s.pattern_root->bind(TOK_KPROBE)->bind_num(TOK_STATEMENT)
      ->bind(TOK_ABSOLUTE)->bind(new kprobe_builder());

  //Hwbkpt based probe
  // NB: we formerly registered the probe point types only if the kernel configuration
  // allowed it.  However, we get better error messages if we allow probes to resolve.
  s.pattern_root->bind(TOK_KERNEL)->bind_num(TOK_HWBKPT)
    ->bind(TOK_HWBKPT_WRITE)->bind(new hwbkpt_builder());
  s.pattern_root->bind(TOK_KERNEL)->bind_str(TOK_HWBKPT)
    ->bind(TOK_HWBKPT_WRITE)->bind(new hwbkpt_builder());
  s.pattern_root->bind(TOK_KERNEL)->bind_num(TOK_HWBKPT)
    ->bind(TOK_HWBKPT_RW)->bind(new hwbkpt_builder());
  s.pattern_root->bind(TOK_KERNEL)->bind_str(TOK_HWBKPT)
    ->bind(TOK_HWBKPT_RW)->bind(new hwbkpt_builder());
  s.pattern_root->bind(TOK_KERNEL)->bind_num(TOK_HWBKPT)
    ->bind_num(TOK_LENGTH)->bind(TOK_HWBKPT_WRITE)->bind(new hwbkpt_builder());
  s.pattern_root->bind(TOK_KERNEL)->bind_num(TOK_HWBKPT)
    ->bind_num(TOK_LENGTH)->bind(TOK_HWBKPT_RW)->bind(new hwbkpt_builder());
  // length supported with address only, not symbol names

  //perf event based probe
  register_tapset_perf(s);
  register_tapset_java(s);
}


vector<derived_probe_group*>
all_session_groups(systemtap_session& s)
{
  vector<derived_probe_group*> g;

#define DOONE(x) \
  if (s. x##_derived_probes) \
    g.push_back ((derived_probe_group*)(s. x##_derived_probes))

  // Note that order *is* important here.  We want to make sure we
  // register (actually run) begin probes before any other probe type
  // is run.  Similarly, when unregistering probes, we want to
  // unregister (actually run) end probes after every other probe type
  // has be unregistered.  To do the latter,
  // c_unparser::emit_module_exit() will run this list backwards.
  DOONE(be);
  DOONE(dwarf);
  DOONE(uprobe);
  DOONE(timer);
  DOONE(profile);
  DOONE(mark);
  DOONE(tracepoint);
  DOONE(kprobe);
  DOONE(hwbkpt);
  DOONE(perf);
  DOONE(hrtimer);
  DOONE(procfs);
  DOONE(netfilter);

  // Another "order is important" item.  We want to make sure we
  // "register" the dummy task_finder probe group after all probe
  // groups that use the task_finder.
  DOONE(utrace);
  DOONE(itrace);
  DOONE(dynprobe);
  DOONE(task_finder);
#undef DOONE
  return g;
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
