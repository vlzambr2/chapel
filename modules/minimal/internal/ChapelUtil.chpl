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

// ChapelUtil.chpl
//
// Internal data structures module
//
module ChapelUtil {

  pragma "no default functions"
  extern record chpl_main_argument {
  }

  //
  // These two are called from the emitted chpl_gen_main(), and
  // defined in the runtime.
  //
  extern proc chpl_rt_preUserCodeHook();
  extern proc chpl_rt_postUserCodeHook();

  //
  // Libraries can't make use of minimal modules, so just declare empty
  // stubs here for the sake of the runtime.
  //
  export proc chpl_libraryModuleLevelSetup() {}
  export proc chpl_libraryModuleLevelCleanup() {}

  // Deinitialization of modules and global variables will not happen.
  proc chpl_addModule(moduleName, deinitFun: chpl_c_fn_ptr) { }

  export proc chpl_deinitModules() { }
}
