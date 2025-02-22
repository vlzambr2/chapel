/*
 * Copyright 2020-2023 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// LocaleModelHelpRuntime.chpl
//
// Provides for declarations common to the locale model runtime
// but that do not have to be the same in order to meet the
// interface.

// They are in this file as a practical matter to avoid code
// duplication. If necessary, a locale model using this file
// should feel free to reimplement them in some other way.
module LocaleModelHelpRuntime {
  private use ChapelStandard, CTypes;

  // The chpl_localeID_t type is used internally.  It should not be exposed to
  // the user.  The runtime defines the actual type, as well as a functional
  // interface for assembling and disassembling chpl_localeID_t values.  This
  // module then provides the interface the compiler-emitted code uses to do
  // the same.

  extern record chpl_localeID_t {
    // We need to know that this is a record type in order to pass it to and
    // return it from runtime functions properly, but we don't need or want
    // to see its contents.
  };

  // runtime stuff about argument bundles
  extern record chpl_comm_on_bundle_t {
  };

  extern record chpl_task_bundle_t {
  };

  extern type chpl_comm_on_bundle_p;

  extern type chpl_task_bundle_p;

  pragma "fn synchronization free"
  extern proc chpl_comm_on_bundle_task_bundle(bundle:chpl_comm_on_bundle_p):chpl_task_bundle_p;

  // Runtime interface for manipulating global locale IDs.
  pragma "fn synchronization free"
  extern
    proc chpl_rt_buildLocaleID(node: chpl_nodeID_t,
                               subloc: chpl_sublocID_t): chpl_localeID_t;

  pragma "fn synchronization free"
  extern
    proc chpl_rt_nodeFromLocaleID(in loc: chpl_localeID_t): chpl_nodeID_t;

  pragma "fn synchronization free"
  extern
    proc chpl_rt_sublocFromLocaleID(in loc: chpl_localeID_t): chpl_sublocID_t;

  // Compiler (and module code) interface for manipulating global locale IDs..
  pragma "insert line file info"
  pragma "always resolve function"
  proc chpl_buildLocaleID(node: chpl_nodeID_t, subloc: chpl_sublocID_t) do
    return chpl_rt_buildLocaleID(node, subloc);

  pragma "insert line file info"
  pragma "always resolve function"
  pragma "codegen for CPU and GPU"
  proc chpl_nodeFromLocaleID(in loc: chpl_localeID_t) do
    return chpl_rt_nodeFromLocaleID(loc);

  pragma "insert line file info"
  pragma "always resolve function"
  pragma "codegen for CPU and GPU"
  proc chpl_sublocFromLocaleID(in loc: chpl_localeID_t) do
    return chpl_rt_sublocFromLocaleID(loc);

  //////////////////////////////////////////
  //
  // support for "on" statements
  //

  //
  // runtime interface
  //
  pragma "insert line file info"
  extern proc chpl_comm_execute_on(loc_id: int, subloc_id: int, fn: int,
                                   args: chpl_comm_on_bundle_p, arg_size: c_size_t);
  pragma "insert line file info"
  extern proc chpl_comm_execute_on_fast(loc_id: int, subloc_id: int, fn: int,
                                        args: chpl_comm_on_bundle_p, args_size: c_size_t);
  pragma "insert line file info"
  extern proc chpl_comm_execute_on_nb(loc_id: int, subloc_id: int, fn: int,
                                      args: chpl_comm_on_bundle_p, args_size: c_size_t);
  pragma "insert line file info"
    extern proc chpl_comm_taskCallFTable(fn: int,
                                         args: chpl_comm_on_bundle_p, args_size: c_size_t,
                                         subloc_id: int): void;
  extern proc chpl_ftable_call(fn: int, args: chpl_comm_on_bundle_p): void;
  extern proc chpl_ftable_call(fn: int, args: chpl_task_bundle_p): void;

  //////////////////////////////////////////
  //
  // support for tasking statements: begin, cobegin, coforall
  //

  //
  // runtime interface
  //
  pragma "insert line file info"
  extern proc chpl_task_addTask(fn: int,
                                args: chpl_task_bundle_p, args_size: c_size_t,
                                subloc_id: int);
  @deprecated("'chpl_task_yield' is deprecated, please use 'currentTask.yieldExecution' instead")
  extern proc chpl_task_yield();

  //
  // Add a task for a begin statement.
  //
  pragma "insert line file info"
  pragma "always resolve function"
  proc chpl_taskAddBegin(subloc_id: int,            // target sublocale
                         fn: int,                   // task body function idx
                         args: chpl_task_bundle_p,  // function args
                         args_size: c_size_t          // args size
                        ) {
    var tls = chpl_task_getInfoChapel();
    var isSerial = chpl_task_data_getSerial(tls);
    if isSerial {
      chpl_ftable_call(fn, args);
    } else {
      chpl_task_data_setup(args, tls);
      chpl_task_addTask(fn, args, args_size, subloc_id);
    }
  }

  //
  // Add a task for a cobegin or coforall statement.
  //
  pragma "insert line file info"
  pragma "always resolve function"
  proc chpl_taskAddCoStmt(subloc_id: int,            // target sublocale
                          fn: int,                   // task body function idx
                          args: chpl_task_bundle_p,  // function args
                          args_size: c_size_t          // args size
                         ) {
    var tls = chpl_task_getInfoChapel();
    var isSerial = chpl_task_data_getSerial(tls);
    if chpl_task_data_getNextCoStmtSerial(tls) {
      isSerial = true;
      chpl_task_data_setNextCoStmtSerial(tls, false);
    }
    if isSerial {
      chpl_ftable_call(fn, args);
    } else {
      chpl_task_data_setup(args, tls);
      chpl_task_addTask(fn, args, args_size, subloc_id);
     }
  }

  // wrap around runtime's chpl__initCopy
  proc chpl__initCopy(initial: chpl_localeID_t,
                      definedConst: bool): chpl_localeID_t {
    // We need an explicit copy constructor because the compiler cannot create
    // a correct one for a record type whose members are not known to it.
    pragma "init copy fn"
    pragma "fn synchronization free"
    extern proc chpl__initCopy_chpl_rt_localeID_t(in initial: chpl_localeID_t): chpl_localeID_t;

    return chpl__initCopy_chpl_rt_localeID_t(initial);
  }

}
