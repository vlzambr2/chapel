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

//
// Compute node topology support: hwloc-based implementation
//
#include "chplrt.h"

#include "chpl-align.h"
#include "chpl-env.h"
#include "chpl-env-gen.h"
#include "chplcgfns.h"
#include "chplsys.h"
#include "chpl-topo.h"
#include "chpl-comm.h"
#include "chpltypes.h"
#include "error.h"
#include "chpl-mem-sys.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hwloc.h"

#ifdef DEBUG
// note: format arg 'f' must be a string constant
#ifdef DEBUG_NODEID
#define _DBG_P(f, ...)                                                  \
        do {                                                            \
          printf("%d:%s:%d: " f "\n", chpl_nodeID, __FILE__, __LINE__,  \
                                      ## __VA_ARGS__);                  \
        } while (0)
#else
#define _DBG_P(f, ...)                                                  \
        do {                                                            \
          printf("%s:%d: " f "\n", __FILE__, __LINE__, ## __VA_ARGS__); \
        } while (0)
#endif
static chpl_bool debug = true;
#else
#define _DBG_P(f, ...)
static chpl_bool debug = false;
#endif

static chpl_bool haveTopology = false;

static hwloc_topology_t topology;

static const struct hwloc_topology_support* topoSupport;
static chpl_bool do_set_area_membind;

static int topoDepth;

static int numNumaDomains;

// A note on core and PU numbering. As per the hwloc documentation, a cpuset
// contains OS indices of PUs. In order to use a cpuset to represent a
// collection of cores and not break this invariant, we represent a core in a
// cpuset with the smallest OS index of its PUs. For example, the physAccSet
// contains the OS indices of the smallest PU for each accessible core.

// Accessible cores and PUs.
static hwloc_cpuset_t physAccSet = NULL;
static hwloc_cpuset_t physReservedSet = NULL;
static hwloc_cpuset_t logAccSet = NULL;
static hwloc_cpuset_t logAllSet = NULL;

// This is used for runtime testing and masks the accessible PUs.
static hwloc_cpuset_t logAccMask = NULL;

static void cpuInfoInit(void);
static void partitionResources(void);

// Accessible NUMA nodes

static hwloc_nodeset_t numaSet = NULL;

// Our root within the overall topology.
static hwloc_obj_t root = NULL;

// Our socket, if applicable.
static hwloc_obj_t socket = NULL;


static hwloc_obj_t getNumaObj(c_sublocid_t);
static void alignAddrSize(void*, size_t, chpl_bool,
                          size_t*, unsigned char**, size_t*);
static void chpl_topo_setMemLocalityByPages(unsigned char*, size_t,
                                            hwloc_obj_t);

// CPU reservation must happen before CPU information is returned to other
// layers.
static chpl_bool okToReserveCPU = true;

static chpl_bool oversubscribed = false;

//
// Error reporting.
//
// CHK_ERR*() must evaluate 'expr' precisely once!
//
static void chk_err_fn(const char*, int, const char*);
static void chk_err_errno_fn(const char*, int, const char*);

#define CHK_ERR(expr) \
  do { if (!(expr)) chk_err_fn(__FILE__, __LINE__, #expr); } while (0)

#define CHK_ERR_ERRNO(expr) \
  do { if (!(expr)) chk_err_errno_fn(__FILE__, __LINE__, #expr); } while (0)

#define REPORT_ERR_ERRNO(expr) \
  chk_err_errno_fn(__FILE__, __LINE__, #expr)

// Partially initialize the topology layer for use during comm initialization.
// The remainder of the initialization is done in chpl_topo_post_comm_init
// after the comm layer has been initialized and we know how many locales
// are running on this node.
//

void chpl_topo_pre_comm_init(char *accessiblePUsMask) {
  //
  // accessibleMask is a string in hwloc "bitmap list" format that
  // specifies which processing units should be considered accessible
  // to this locale. It is intended for testing purposes only and
  // should be NULL in production code.

  //
  // We only load hwloc topology information in configurations where
  // the locale model is other than "flat" or the tasking is based on
  // Qthreads (which will use the topology we load).  We don't use
  // it otherwise (so far) because loading it is somewhat expensive.
  //
  if (strcmp(CHPL_LOCALE_MODEL, "flat") != 0
      || strcmp(CHPL_TASKS, "qthreads") == 0) {
    haveTopology = true;
  } else {
    haveTopology = false;
    return;
  }

  //
  // Allocate and initialize topology object.
  //
  CHK_ERR_ERRNO(hwloc_topology_init(&topology) == 0);

  int flags = HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED;
  flags |= HWLOC_TOPOLOGY_FLAG_IMPORT_SUPPORT; // for testing
  CHK_ERR_ERRNO(hwloc_topology_set_flags(topology, flags) == 0);

  CHK_ERR_ERRNO(hwloc_topology_set_all_types_filter(topology,
                                            HWLOC_TYPE_FILTER_KEEP_ALL) == 0);

  //
  // Perform the topology detection.
  //
  CHK_ERR_ERRNO(hwloc_topology_load(topology) == 0);

  //
  // What is supported?
  //
  topoSupport = hwloc_topology_get_support(topology);

  //
  // TODO: update comment
  // For now, don't support setting memory locality when comm=ugni or
  // comm=gasnet, seg!=everything.  Those are the two configurations in
  // which we use hugepages and/or memory registered with the comm
  // interface, both of which may be a problem for the set-membind call.
  // We will have other ways to achieve locality for these configs in
  // the future.
  //
  do_set_area_membind = true;
  if ((strcmp(CHPL_COMM, "gasnet") == 0
       && strcmp(CHPL_GASNET_SEGMENT, "everything") != 0)) {
      do_set_area_membind = false;
  }

  //
  // We need depth information.
  //
  topoDepth = hwloc_topology_get_depth(topology);

  //
  // By default our root is the root of the topology.
  //

  root = hwloc_get_root_obj(topology);

  if (accessiblePUsMask != NULL) {
    CHK_ERR_ERRNO((logAccMask = hwloc_bitmap_alloc()) != NULL);
    CHK_ERR(hwloc_bitmap_list_sscanf(logAccMask, accessiblePUsMask) == 0);
    if (debug) {
      char buf[1024];
      hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccMask);
      _DBG_P("logAccMask: %s", buf);
    }
  }
  cpuInfoInit();
}

//
// Finish initializing the topology layer after the comm layer has been
// initialized.
//
void chpl_topo_post_comm_init(void) {
  partitionResources();
}


void chpl_topo_exit(void) {
  if (!haveTopology) {
    return;
  }

  if (physAccSet != NULL) {
    hwloc_bitmap_free(physAccSet);
    physAccSet = NULL;
  }
  if (physReservedSet != NULL) {
    hwloc_bitmap_free(physReservedSet);
    physReservedSet = NULL;
  }
  if (logAccSet != NULL) {
    hwloc_bitmap_free(logAccSet);
    logAccSet = NULL;
  }
  if (logAllSet != NULL) {
    hwloc_bitmap_free(logAllSet);
    logAllSet = NULL;
  }
  if (numaSet != NULL) {
    hwloc_bitmap_free(numaSet);
    numaSet = NULL;
  }
  if (logAccMask != NULL) {
    hwloc_bitmap_free(logAccMask);
    logAccMask = NULL;
  }

  hwloc_topology_destroy(topology);
}


void* chpl_topo_getHwlocTopology(void) {
  return (haveTopology) ? topology : NULL;
}

//
// How many CPUs (cores or PUs) are there?
//
static int numCPUsPhysAcc = -1;
static int numCPUsPhysAll = -1;
static int numCPUsLogAcc  = -1;
static int numCPUsLogAll  = -1;
static int numSockets = -1;

int chpl_topo_getNumCPUsPhysical(chpl_bool accessible_only) {
  okToReserveCPU = false;
  int cpus = (accessible_only) ? numCPUsPhysAcc : numCPUsPhysAll;
  if (cpus == -1) {
    chpl_error("number of cpus is uninitialized", 0, 0);
  }
  return cpus;
}


int chpl_topo_getNumCPUsLogical(chpl_bool accessible_only) {
  okToReserveCPU = false;
  int cpus = (accessible_only) ? numCPUsLogAcc : numCPUsLogAll;
  if (cpus == -1) {
    chpl_error("number of cpus is uninitialized", 0, 0);
  }
  return cpus;
}


#define NEXT_OBJ(cpuset, type, obj)                                \
  hwloc_get_next_obj_inside_cpuset_by_type(topology, (cpuset),     \
                                           (type), (obj))

// Filter any PUs from the cpuset whose entry in ignoreKinds is true
static void filterPUsByKind(int numKinds, chpl_bool *ignoreKinds,
                         hwloc_cpuset_t cpuset) {

  // filtering only makes sense if there is more than one kind of PU
  if (numKinds > 1) {
    for (hwloc_obj_t pu = NEXT_OBJ(cpuset, HWLOC_OBJ_PU, NULL);
         pu != NULL;
         pu = NEXT_OBJ(cpuset, HWLOC_OBJ_PU, pu)) {
      if (debug) {
        char buf[1024];
        hwloc_bitmap_list_snprintf(buf, sizeof(buf), pu->cpuset);
        _DBG_P("filterPUsByKind PU cpuset: %s", buf);
      }
      int kind = hwloc_cpukinds_get_by_cpuset(topology, pu->cpuset, 0);
      _DBG_P("kind = %d, numKinds = %d", kind, numKinds);
      CHK_ERR_ERRNO((kind >= 0) && (kind < numKinds));
      if (ignoreKinds[kind]) {
        hwloc_bitmap_andnot(cpuset, cpuset, pu->cpuset);
      }
    }
  }
}

//
// Initializes information about all CPUs (cores and PUs) from
// the topology. The accessible CPUs are initialized as a side-effect,
// but they aren't partitioned until partitionResources is called.
//

static void cpuInfoInit(void) {
  _DBG_P("cpuInfoInit");

  CHK_ERR_ERRNO((physAccSet = hwloc_bitmap_alloc()) != NULL);
  CHK_ERR_ERRNO((physReservedSet = hwloc_bitmap_alloc()) != NULL);
  CHK_ERR_ERRNO((numaSet = hwloc_bitmap_alloc()) != NULL);


  // Determine which kind(s) of PUs we are supposed to use.
  // hwloc returns kinds sorted by efficiency, least efficient
  // (more performant) last. Currently, we put them into two
  // groups, most performant ("performance") and lump all the
  // rest into "efficiency".

  int numKinds;
  CHK_ERR_ERRNO((numKinds = hwloc_cpukinds_get_nr(topology, 0)) >= 0);
  _DBG_P("There are %d kinds of PUs", numKinds);
  chpl_bool *ignoreKinds = NULL;
  if (numKinds > 1) {
    ignoreKinds = sys_calloc(numKinds, sizeof(*ignoreKinds));
    CHK_ERR(ignoreKinds);
    // there are multiple kinds of PUs
    const char *kindStr = chpl_env_rt_get("USE_PU_KIND", "performance");
    if (!strcasecmp(kindStr, "performance")) {
      // use only performance PUs. This is the default.
      _DBG_P("using only performance PUs");
      for (int i = 0; i < numKinds - 1; i++) {
        ignoreKinds[i] = true;
      }
    } else if (!strcasecmp(kindStr, "efficiency")) {
      // use only efficiency PUs
      _DBG_P("using only efficiency PUs");
      ignoreKinds[numKinds-1] = true;
    } else if (!strcasecmp(kindStr, "all")) {
      // do nothing, we'll use all kinds of PUs
      _DBG_P("using all PUs");
    } else {
      char msg[200];
      snprintf(msg, sizeof(msg),
               "\"%s\" is not a valid value for CHPL_RT_USE_PU_KIND.\n"
               "Must be one of \"performance\", \"efficiency\", or \"all\".",
               kindStr);
        chpl_error(msg, 0, 0);
    }
  }

  // accessible PUs

  logAccSet = hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topology));
  if (logAccMask) {
    // Modify accessible PUs for testing purposes.
    hwloc_bitmap_and(logAccSet, logAccSet, logAccMask);
  }
  if (debug) {
    char buf[1024];
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccSet);
    _DBG_P("logAccSet after masking: %s", buf);
  }

  _DBG_P("filtering logAccSet");
  filterPUsByKind(numKinds, ignoreKinds, logAccSet);
  numCPUsLogAcc = hwloc_bitmap_weight(logAccSet);
  _DBG_P("numCPUsLogAcc = %d", numCPUsLogAcc);


  // accessible cores

  int maxPusPerAccCore = 0;

  for (hwloc_obj_t core = NEXT_OBJ(logAccSet, HWLOC_OBJ_CORE, NULL);
       core != NULL;
       core = NEXT_OBJ(logAccSet, HWLOC_OBJ_CORE, core)) {
    // filter the core's PUs
    hwloc_cpuset_t cpuset = NULL;
    CHK_ERR_ERRNO((cpuset = hwloc_bitmap_dup(core->cpuset)) != NULL);
      char buf[1024];
      hwloc_bitmap_list_snprintf(buf, sizeof(buf), cpuset);
      _DBG_P("core cpuset: %s", buf);
    // filter the core's PUs in case they are hybrid
    _DBG_P("filtering core's cpuset");
    filterPUsByKind(numKinds, ignoreKinds, cpuset);

    // determine the max # PUs in a core
    int numPus = hwloc_bitmap_weight(cpuset);
    if (numPus > maxPusPerAccCore) {
      maxPusPerAccCore = numPus;
    }
    // use the smallest PU index to represent the core in physAccSet
    int smallest = hwloc_bitmap_first(cpuset);
    CHK_ERR(smallest != -1);
    hwloc_bitmap_set(physAccSet, smallest);
    hwloc_bitmap_free(cpuset);
  }

  if (ignoreKinds) {
    sys_free(ignoreKinds);
    ignoreKinds = NULL;
  }

  numCPUsPhysAcc = hwloc_bitmap_weight(physAccSet);
  if (numCPUsPhysAcc == 0) {
    chpl_error("No useable cores.", 0, 0);
  }

  //
  // all cores
  //

  logAllSet = hwloc_bitmap_dup(hwloc_topology_get_complete_cpuset(topology));
  numCPUsLogAll = hwloc_bitmap_weight(logAllSet);
  CHK_ERR(numCPUsLogAll > 0);
  _DBG_P("numCPUsLogAll = %d", numCPUsLogAll);

  if (numCPUsLogAll == numCPUsLogAcc) {
    // All PUs and therefore all cores are accessible
    numCPUsPhysAll = numCPUsPhysAcc;
  } else {
    // Some cores are inaccessible. We estimate their number by
    // assuming they all have the maximum number of PUs.
    numCPUsPhysAll = numCPUsLogAll / maxPusPerAccCore;
  }
  CHK_ERR(numCPUsPhysAll > 0);
  _DBG_P("numCPUsPhysAll = %d", numCPUsPhysAll);
  _DBG_P("numCPUsPhysAcc = %d", numCPUsPhysAcc);

  if (debug) {
    char buf[1024];
    _DBG_P("numCPUsLogAll: %d", numCPUsLogAll);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccSet);
    _DBG_P("numCPUsLogAcc: %d logAccSet: %s", numCPUsLogAcc,
           buf);

    _DBG_P("numCPUsPhysAll: %d", numCPUsPhysAll);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), physAccSet);
    _DBG_P("numCPUsPhysAcc: %d physAccSet: %s", numCPUsPhysAcc,
           buf);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), numaSet);
    _DBG_P("numaSet: %s", buf);

    hwloc_const_cpuset_t set;

    set = hwloc_topology_get_allowed_cpuset(topology);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), set);
    _DBG_P("allowed cpuset: %s", buf);

    set = hwloc_topology_get_complete_cpuset(topology);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), set);
    _DBG_P("complete cpuset: %s", buf);

    set = hwloc_topology_get_topology_cpuset(topology);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), set);
    _DBG_P("topology cpuset: %s", buf);

  }
}

//
// Partitions resources when running with co-locales. Currently, only
// partitioning based on sockets is supported.
//

static void partitionResources(void) {
  _DBG_P("partitionResources");
  numSockets = hwloc_get_nbobjs_inside_cpuset_by_type(topology,
                      root->cpuset, HWLOC_OBJ_PACKAGE);
  _DBG_P("numSockets = %d", numSockets);

  int numLocalesOnNode = chpl_get_num_locales_on_node();
  int expectedLocalesOnNode = chpl_env_rt_get_int("LOCALES_PER_NODE", 1);
  chpl_bool useSocket = chpl_env_rt_get_bool("USE_SOCKET", false);
  int rank = chpl_get_local_rank();
  _DBG_P("numLocalesOnNode = %d", numLocalesOnNode);
  _DBG_P("expectedLocalesOnNode = %d", expectedLocalesOnNode);
  _DBG_P("rank = %d", rank);
  _DBG_P("useSocket = %d", useSocket);
  if (numLocalesOnNode > 1) {
    oversubscribed = true;
  }
  if ((expectedLocalesOnNode > 1) || useSocket) {
    // We get our own socket if all cores are accessible, we know our local
    // rank, and the number of locales on the node is less than or equal to
    // the number of sockets. It is an error if the number of locales on the
    // node is greater than the number of sockets and CHPL_RT_LOCALES_PER_NODE
    // is set, otherwise we are oversubscribed.

    // TODO: The oversubscription determination is incorrect. A node is only
    // oversubscribed if locales are sharing cores. Need to figure out how
    // to determine this accurately.

    if (numCPUsPhysAcc == numCPUsPhysAll) {
      if (numLocalesOnNode <= numSockets) {
        if (rank != -1) {
          // Use the socket whose logical index corresponds to our local rank.
          // See getSocketNumber below if you change this.
          _DBG_P("confining ourself to socket %d", rank);
          socket = hwloc_get_obj_inside_cpuset_by_type(topology,
                                    root->cpuset, HWLOC_OBJ_PACKAGE, rank);
          CHK_ERR(socket != NULL);

          // Limit the accessible cores and PUs to those in our socket.

          hwloc_bitmap_and(logAccSet, logAccSet, socket->cpuset);
          numCPUsLogAcc = hwloc_bitmap_weight(logAccSet);
          CHK_ERR(numCPUsLogAcc > 0);

          hwloc_bitmap_and(physAccSet, physAccSet, socket->cpuset);
          numCPUsPhysAcc = hwloc_bitmap_weight(physAccSet);
          CHK_ERR(numCPUsPhysAcc > 0);

          if (debug) {
            char buf[1024];
            hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccSet);
            _DBG_P("numCPUsLogAcc: %d logAccSet: %s", numCPUsLogAcc, buf);
            hwloc_bitmap_list_snprintf(buf, sizeof(buf), physAccSet);
            _DBG_P("numCPUsPhysAcc: %d physAccSet: %s", numCPUsPhysAcc, buf);
          }
          root = socket;
          oversubscribed = false;
        }
      } else if (expectedLocalesOnNode > 0) {
        char msg[100];
        snprintf(msg, sizeof(msg), "The number of locales on the node is "
                 "greater than the number of sockets (%d > %d).",
                 numLocalesOnNode, numSockets);
        chpl_error(msg, 0, 0);
      }
    }
  }

  // CHPL_RT_OVERSUBSCRIBED overrides oversubscription determination

  oversubscribed = chpl_env_rt_get_bool("OVERSUBSCRIBED", oversubscribed);

  if ((verbosity >= 2) && (chpl_nodeID == 0)) {
    printf("oversubscribed = %s\n", oversubscribed ? "True" : "False");
  }

  // Find the NUMA nodes.

  hwloc_cpuset_to_nodeset(topology, logAccSet, numaSet);
  numNumaDomains = hwloc_bitmap_weight(numaSet);
  _DBG_P("numNumaDomains %d", numNumaDomains);
  if (debug) {
    char buf[1024];
    _DBG_P("numCPUsLogAll: %d", numCPUsLogAll);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccSet);
    _DBG_P("numCPUsLogAcc: %d logAccSet: %s", numCPUsLogAcc,
           buf);

    _DBG_P("numCPUsPhysAll: %d", numCPUsPhysAll);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), physAccSet);
    _DBG_P("numCPUsPhysAcc: %d physAccSet: %s", numCPUsPhysAcc,
           buf);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), numaSet);
    _DBG_P("numaSet: %s", buf);

    hwloc_const_cpuset_t set;

    set = hwloc_topology_get_allowed_cpuset(topology);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), set);
    _DBG_P("allowed cpuset: %s", buf);

    set = hwloc_topology_get_complete_cpuset(topology);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), set);
    _DBG_P("complete cpuset: %s", buf);

    set = hwloc_topology_get_topology_cpuset(topology);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), set);
    _DBG_P("topology cpuset: %s", buf);
  }
}

#undef NEXT_OBJ

// If we are running in a socket then cpuInfoInit will assign each locale to
// the socket whose logical index is equal to the locale's local rank. This
// function returns the socket number for the given locale. Right now it's
// the identity mapping, but should be changed if the way cpuInfoInit does
// the mapping is changed.
static
int getSocketNumber(int localRank) {
  int result = -1;
  if (socket != NULL) {
    result = localRank;
  }
  return result;
}

void chpl_topo_post_args_init(void) {
  if ((verbosity >= 2) && (socket != NULL)) {
    printf("%d: using socket %d\n", chpl_nodeID,
           socket->logical_index);
  }
}

//
// Fills the "cpus" array with the hwloc "cpuset" (a bitmap whose bits are
// set according to CPU physical OS indexes).
//
static
int getCPUs(hwloc_cpuset_t cpuset, int *cpus, int size) {
  int count = 0;
  int id;
  hwloc_bitmap_foreach_begin(id, cpuset) {
    if (count == size) {
      break;
    }
    cpus[count++] = id;
  } hwloc_bitmap_foreach_end();
  return count;
}


//
// Fills the "cpus" array with up to "count" physical OS indices of the
// accessible cores or PUs. If "physical" is true, then "cpus" contains
// core indices, otherwise it contains PU indices. Returns the number
// of indices in the "cpus" array.
//
int chpl_topo_getCPUs(chpl_bool physical, int *cpus, int count) {
  // Initializes CPU information.
  okToReserveCPU = false;
  return getCPUs(physical ? physAccSet : logAccSet, cpus, count);
}


int chpl_topo_getNumNumaDomains(void) {
  return numNumaDomains;
}


void chpl_topo_setThreadLocality(c_sublocid_t subloc) {
  hwloc_cpuset_t cpuset;
  int flags;

  _DBG_P("chpl_topo_setThreadLocality(%d)", (int) subloc);

  if (!haveTopology) {
    return;
  }

  if (!topoSupport->cpubind->set_thread_cpubind)
    return;

  CHK_ERR_ERRNO((cpuset = hwloc_bitmap_alloc()) != NULL);

  hwloc_cpuset_from_nodeset(topology, cpuset,
                            getNumaObj(subloc)->nodeset);

  // Only use accessible CPUs.

  hwloc_bitmap_and(cpuset, cpuset, logAccSet);

  flags = HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT;
  CHK_ERR_ERRNO(hwloc_set_cpubind(topology, cpuset, flags) == 0);
  if (debug) {
    char buf[1024];
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), cpuset);
    _DBG_P("chpl_topo_setThreadLocality(%d): %s", (int) subloc, buf);
  }
  hwloc_bitmap_free(cpuset);
}


c_sublocid_t chpl_topo_getThreadLocality(void) {
  hwloc_cpuset_t cpuset;
  hwloc_nodeset_t nodeset;
  int flags;
  int node;

  if (!haveTopology) {
    return c_sublocid_any;
  }

  if (!topoSupport->cpubind->get_thread_cpubind) {
    return c_sublocid_any;
  }

  CHK_ERR_ERRNO((cpuset = hwloc_bitmap_alloc()) != NULL);
  CHK_ERR_ERRNO((nodeset = hwloc_bitmap_alloc()) != NULL);

  flags = HWLOC_CPUBIND_THREAD;
  CHK_ERR_ERRNO(hwloc_get_cpubind(topology, cpuset, flags) == 0);

  hwloc_cpuset_to_nodeset(topology, cpuset, nodeset);

  node = hwloc_bitmap_first(nodeset);

  hwloc_bitmap_free(nodeset);
  hwloc_bitmap_free(cpuset);

  return node;
}


void chpl_topo_setMemLocality(void* p, size_t size, chpl_bool onlyInside,
                              c_sublocid_t subloc) {
  size_t pgSize;
  unsigned char* pPgLo;
  size_t nPages;

  _DBG_P("chpl_topo_setMemLocality(%p, %#zx, onlyIn=%s, %d)",
         p, size, (onlyInside ? "T" : "F"), (int) subloc);

  if (!haveTopology) {
    return;
  }

  alignAddrSize(p, size, onlyInside, &pgSize, &pPgLo, &nPages);

  _DBG_P("    localize %p, %#zx bytes (%#zx pages)",
         pPgLo, nPages * pgSize, nPages);

  if (nPages == 0)
    return;

  chpl_topo_setMemLocalityByPages(pPgLo, nPages * pgSize, getNumaObj(subloc));
}


void chpl_topo_setMemSubchunkLocality(void* p, size_t size,
                                      chpl_bool onlyInside,
                                      size_t* subchunkSizes) {
  size_t pgSize;
  unsigned char* pPgLo;
  size_t nPages;
  int i;
  size_t pg;
  size_t pgNext;

  _DBG_P("chpl_topo_setMemSubchunkLocality(%p, %#zx, onlyIn=%s)",
         p, size, (onlyInside ? "T" : "F"));

  if (!haveTopology) {
    return;
  }

  alignAddrSize(p, size, onlyInside, &pgSize, &pPgLo, &nPages);

  _DBG_P("    localize %p, %#zx bytes (%#zx pages)",
         pPgLo, nPages * pgSize, nPages);

  if (nPages == 0)
    return;

  for (i = 0, pg = 0; i < numNumaDomains; i++, pg = pgNext) {
    if (i == numNumaDomains - 1)
      pgNext = nPages;
    else
      pgNext = 1 + (nPages * (i + 1) - 1) / numNumaDomains;
    chpl_topo_setMemLocalityByPages(pPgLo + pg * pgSize,
                                    (pgNext - pg) * pgSize, getNumaObj(i));
    if (subchunkSizes != NULL) {
      subchunkSizes[i] = (pgNext - pg) * pgSize;
    }
  }
}


void chpl_topo_touchMemFromSubloc(void* p, size_t size, chpl_bool onlyInside,
                                  c_sublocid_t subloc) {
  size_t pgSize;
  unsigned char* pPgLo;
  size_t nPages;
  hwloc_cpuset_t cpuset;
  int flags;

  _DBG_P("chpl_topo_touchMemFromSubloc(%p, %#zx, onlyIn=%s, %d)",
         p, size, (onlyInside ? "T" : "F"), (int) subloc);

  if (!haveTopology
      || !topoSupport->cpubind->get_thread_cpubind
      || !topoSupport->cpubind->set_thread_cpubind) {
    return;
  }

  alignAddrSize(p, size, onlyInside, &pgSize, &pPgLo, &nPages);

  _DBG_P("    localize %p, %#zx bytes (%#zx pages)",
         pPgLo, nPages * pgSize, nPages);

  if (nPages == 0)
    return;

  CHK_ERR_ERRNO((cpuset = hwloc_bitmap_alloc()) != NULL);

  flags = HWLOC_CPUBIND_THREAD;
  CHK_ERR_ERRNO(hwloc_set_cpubind(topology, cpuset, flags) == 0);

  chpl_topo_setThreadLocality(subloc);

  {
    size_t pg;
    for (pg = 0; pg < nPages; pg++) {
      pPgLo[pg * pgSize] = 0;
    }
  }

  flags = HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT;
  CHK_ERR_ERRNO(hwloc_set_cpubind(topology, cpuset, flags) == 0);

  hwloc_bitmap_free(cpuset);
}


static inline
hwloc_obj_t getNumaObj(c_sublocid_t subloc) {
  int id;
  int count = 0;
  hwloc_bitmap_foreach_begin(id, numaSet) {
    if (count == subloc) {
      break;
    }
    count++;
  } hwloc_bitmap_foreach_end();
  return hwloc_get_numanode_obj_by_os_index(topology, id);
}


static inline
void alignAddrSize(void* p, size_t size, chpl_bool onlyInside,
                   size_t* p_pgSize, unsigned char** p_pPgLo,
                   size_t* p_nPages) {
  unsigned char* pCh = (unsigned char*) p;
  size_t pgSize = chpl_getHeapPageSize();
  size_t pgMask = pgSize - 1;
  unsigned char* pPgLo;
  size_t nPages;

  if (onlyInside) {
    pPgLo = round_up_to_mask_ptr(pCh, pgMask);
    if (size < pPgLo - pCh)
      nPages = 0;
    else
      nPages = round_down_to_mask(size - (pPgLo - pCh), pgMask) / pgSize;
  } else {
    pPgLo = round_down_to_mask_ptr(pCh, pgMask);
    nPages = round_up_to_mask(size + (pCh - pPgLo), pgMask) / pgSize;
  }

  *p_pgSize = pgSize;
  *p_pPgLo = pPgLo;
  *p_nPages = nPages;
}


void chpl_topo_interleaveMemLocality(void* p, size_t size) {
  int flags;

  if (!haveTopology) {
    return;
  }

  if (!topoSupport->membind->set_area_membind ||
      !topoSupport->membind->interleave_membind) {
    return;
  }

  hwloc_bitmap_t set;
  set = hwloc_bitmap_dup(root->cpuset);

  flags = 0;
  CHK_ERR_ERRNO(hwloc_set_area_membind(topology, p, size, set, HWLOC_MEMBIND_INTERLEAVE, flags) == 0);
}


//
// p must be page aligned and the page size must evenly divide size
//
static
void chpl_topo_setMemLocalityByPages(unsigned char* p, size_t size,
                                     hwloc_obj_t numaObj) {
  int flags;

  if (!haveTopology) {
    return;
  }

  if (!topoSupport->membind->set_area_membind
      || !do_set_area_membind)
    return;

  _DBG_P("hwloc_set_area_membind(%p, %#zx, %d)", p, size,
         (int) hwloc_bitmap_first(numaObj->nodeset));

  flags = HWLOC_MEMBIND_MIGRATE | HWLOC_MEMBIND_STRICT;
  CHK_ERR_ERRNO(hwloc_set_area_membind(topology, p, size,
                                               numaObj->nodeset,
                                               HWLOC_MEMBIND_BIND, flags)
                == 0);
}


c_sublocid_t chpl_topo_getMemLocality(void* p) {
  int flags;
  hwloc_nodeset_t nodeset;
  int node;

  if (!haveTopology) {
    return c_sublocid_any;
  }

  if (!topoSupport->membind->get_area_memlocation) {
    return c_sublocid_any;
  }

  if (p == NULL) {
    return c_sublocid_any;
  }

  CHK_ERR_ERRNO((nodeset = hwloc_bitmap_alloc()) != NULL);

  flags = HWLOC_MEMBIND_BYNODESET;
  CHK_ERR_ERRNO(hwloc_get_area_memlocation(topology, p, 1, nodeset, flags)
                == 0);

  node = hwloc_bitmap_first(nodeset);
  if (!isActualSublocID(node)) {
    node = c_sublocid_any;
  }

  hwloc_bitmap_free(nodeset);

  return node;
}


//
// Reserves a physical CPU (core) and returns its hwloc OS index. The core and
// its PUs will not be returned by chpl_topo_getCPUs,
// chpl_topo_getNumCPUsPhysical, and chpl_topo_getNumCPUsLogical. Must be
// called before those functions. Will not reserve a core if CPU binding is
// not supported on this platform or if there is only one unreserved core.
//
// Returns OS index of reserved core, -1 otherwise
//
int
chpl_topo_reserveCPUPhysical(void) {
  int id = -1;
  _DBG_P("topoSupport->cpubind->set_thisthread_cpubind: %d",
         topoSupport->cpubind->set_thisthread_cpubind);
  _DBG_P("numCPUsPhysAcc: %d", numCPUsPhysAcc);
  if (okToReserveCPU) {
    if ((topoSupport->cpubind->set_thisthread_cpubind) &&
        (numCPUsPhysAcc > 1)) {

      if (debug) {
        char buf[1024];
        _DBG_P("chpl_topo_reserveCPUPhysical before");
        hwloc_bitmap_list_snprintf(buf, sizeof(buf), physAccSet);
        _DBG_P("physAccSet: %s", buf);
        hwloc_bitmap_list_snprintf(buf, sizeof(buf), physReservedSet);
        _DBG_P("physReservedSet: %s", buf);
        hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccSet);
        _DBG_P("logAccSet: %s", buf);
      }
      // Reserve the highest-numbered core.
      id = hwloc_bitmap_last(physAccSet);
      if (id >= 0) {

        // Find the core's object in the topology so we can reserve its PUs.
        hwloc_obj_t pu, core;
        CHK_ERR_ERRNO(pu = hwloc_get_pu_obj_by_os_index(topology, id));
        CHK_ERR_ERRNO(core = hwloc_get_ancestor_obj_by_type(topology,
                                                            HWLOC_OBJ_CORE,
                                                            pu));
        // Reserve the core.
        hwloc_bitmap_andnot(physAccSet, physAccSet, pu->cpuset);
        numCPUsPhysAcc = hwloc_bitmap_weight(physAccSet);
        hwloc_bitmap_or(physReservedSet, physReservedSet, pu->cpuset);
        CHK_ERR(numCPUsPhysAcc > 0);

        // Reserve the core's PUs.
        hwloc_bitmap_andnot(logAccSet, logAccSet, core->cpuset);
        numCPUsLogAcc = hwloc_bitmap_weight(logAccSet);
        CHK_ERR(numCPUsLogAcc > 0);

        _DBG_P("reserved core %d", id);
      }
    }
  } else {
    _DBG_P("okToReserveCPU is false");
  }

  if (debug) {
    char buf[1024];
    _DBG_P("chpl_topo_reserveCPUPhysical %d", id);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), physAccSet);
    _DBG_P("physAccSet: %s", buf);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), physReservedSet);
    _DBG_P("physReservedSet: %s", buf);
    hwloc_bitmap_list_snprintf(buf, sizeof(buf), logAccSet);
    _DBG_P("logAccSet: %s", buf);
  }
  return id;
}


//
// Binds the current thread to the specified CPU. The CPU must
// have previously been reserved via chpl_topo_reserveCPUPhysical.
//
// Returns 0 on success, 1 otherwise
//
int chpl_topo_bindCPU(int id) {
  int status = 1;
  if (hwloc_bitmap_isset(physReservedSet, id) &&
      (topoSupport->cpubind->set_thisthread_cpubind)) {
    int flags = HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT;
    hwloc_cpuset_t cpuset;
    CHK_ERR_ERRNO((cpuset = hwloc_bitmap_alloc()) != NULL);
    hwloc_bitmap_set(cpuset, id);
    CHK_ERR_ERRNO(hwloc_set_cpubind(topology, cpuset, flags) == 0);
    hwloc_bitmap_free(cpuset);
    status = 0;
  }
  _DBG_P("chpl_topo_bindCPU id: %d status: %d", id, status);
  return status;
}

chpl_bool chpl_topo_isOversubscribed(void) {
  _DBG_P("oversubscribed = %s", oversubscribed ? "True" : "False");
  return oversubscribed;
}
//
// Information used to sort NICs and to track which ones have already
// been assigned to a locale.
//
typedef struct nic_info_t {
  int         socket;
  hwloc_obj_t obj;
  chpl_bool   assigned;
} nic_info_t;

//
// Comparison function for sort. Sorts based on socket then PCI address.
//
static int compareNics(const void *a, const void *b)
{
  nic_info_t *nicA = (nic_info_t *) a;
  nic_info_t *nicB = (nic_info_t *) b;

  int result;

  result = nicA->socket - nicB->socket;
  if (result == 0) {
    struct hwloc_pcidev_attr_s *attrA = &(nicA->obj->attr->pcidev);
    struct hwloc_pcidev_attr_s *attrB = &(nicB->obj->attr->pcidev);
    result = attrA->domain - attrB->domain;
    if (result == 0) {
      result = attrA->bus - attrB->bus;
      if (result == 0) {
        result = attrA->dev - attrB->dev;
        if (result == 0) {
          result = attrA->func - attrB->func;
        }
      }
    }
  }
  return result;
}

//
// Given a NIC, determines which NIC of the same type (same vendor and device)
// is the best to use. The "best" NIC is one in the same socket as this
// locale. If there isn't a NIC in our socket then use an "extra" NIC if some
// sockets have more than one, otherwise use an already-assigned NIC. In
// either case choose a NIC in a round-robin fashion from those locales that
// do not have a NIC in their socket.
//

chpl_topo_pci_addr_t *chpl_topo_selectNicByType(chpl_topo_pci_addr_t *inAddr,
                                                chpl_topo_pci_addr_t *outAddr)
{
  hwloc_obj_t nic = NULL;
  struct hwloc_pcidev_attr_s *nicAttr;
  chpl_topo_pci_addr_t *result = NULL;
  nic_info_t *nics = NULL;
  int *assignedNics = NULL;

  if (root->type != HWLOC_OBJ_PACKAGE) {
    // We aren't running in a socket, so we don't care which NIC is used.
    goto done;
  }

  // find the PCI object corresponding to the specified NIC
  for (hwloc_obj_t obj = hwloc_get_next_pcidev(topology, NULL);
       obj != NULL;
       obj = hwloc_get_next_pcidev(topology, obj)) {
    if (obj->type == HWLOC_OBJ_PCI_DEVICE) {
      struct hwloc_pcidev_attr_s *attr = &(obj->attr->pcidev);
      if ((attr->domain == inAddr->domain) && (attr->bus == inAddr->bus) &&
          (attr->dev == inAddr->device) && (attr->func == inAddr->function)) {
        nic = obj;
        break;
      }
    }
  }
  if (nic == NULL) {
    _DBG_P("Could not find NIC %04x:%02x:%02x.%x", inAddr->domain,
           inAddr->bus, inAddr->device, inAddr->function);
    goto done;
  }

  // Find all the NICS of the same vendor and device as the specified NIC and
  // sort them by socket and PCI address.

  nicAttr = &(nic->attr->pcidev);
  int maxNics = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PCI_DEVICE);
  CHK_ERR(nics = sys_calloc(maxNics, sizeof(*nics)));
  int numNics = 0;

  for (hwloc_obj_t obj = hwloc_get_next_pcidev(topology, NULL);
       obj != NULL;
       obj = hwloc_get_next_pcidev(topology, obj)) {

    if (obj->type == HWLOC_OBJ_PCI_DEVICE) {
      struct hwloc_pcidev_attr_s *attr = &(obj->attr->pcidev);
      if ((attr->vendor_id == nicAttr->vendor_id) &&
          (attr->device_id == nicAttr->device_id)) {
        hwloc_obj_t sobj = hwloc_get_ancestor_obj_by_type(topology,
                                                          HWLOC_OBJ_PACKAGE,
                                                          obj);
        if (sobj == NULL) {
          _DBG_P("Could not find socket for NIC %04x:%02x:%02x.%x",
                 attr->domain, attr->bus, attr->dev, attr->func);
          goto done;
        }
        nics[numNics].socket = sobj->logical_index;
        nics[numNics].obj = obj;
        nics[numNics].assigned = false;
        numNics++;
      }
    }
  }
  qsort(nics, numNics, sizeof(*nics), compareNics);

  // Use the first NIC in our socket if there is one.

  for (int i = 0; i < numNics; i++) {
    if (nics[i].socket == root->logical_index) {
      nic = nics[i].obj;
      goto done;
    }
  }

  // There isn't a NIC in our socket. Use the nth unassigned NIC, where
  // n is our rank among the locales that don't have NICs, modulo
  // the number of unassigned NICs. Otherwise use the nth assigned NIC.

  int numLocalesOnNode = chpl_get_num_locales_on_node();
  CHK_ERR(assignedNics = sys_calloc(numLocalesOnNode, sizeof(*assignedNics)));

  for (int i = 0; i < numLocalesOnNode; i++) {
    assignedNics[i] = -1;
  }

  // Look for extra (unassigned) NICs. Any NIC whose socket number matches
  // a locale's socket number will be assigned above. The rest are extra.

  int numAssignedNics = 0;
  for (int lid = 0; lid < numLocalesOnNode; lid++) {
    for (int nid = 0; nid < numNics; nid++) {
      if (nics[nid].socket == getSocketNumber(lid)) {
        assignedNics[lid] = nid;
        nics[nid].assigned = true;
        numAssignedNics++;
        break;
      }
    }
  }

  // Determine our rank within the locales that do not have a NIC assigned.

  int unmatchedLocales = 0;
  int unassignedRank = -1;
  int rank = chpl_get_local_rank();
  for (int lid = 0; lid < numLocalesOnNode; lid++) {
    if (lid == rank) {
      unassignedRank = unmatchedLocales;
      break;
    }
    if (assignedNics[lid] == -1) {
      unmatchedLocales++;
    }
  }
  CHK_ERR(unassignedRank != -1);

  if (numAssignedNics == numNics) {

    // All NICs are assigned, we'll have to share one.

    nic = nics[unassignedRank % numNics].obj;
  } else {

    // Use an unassigned NIC, perhaps sharing one if necessary.
    // Note that this can lead to unbalanced loads, but should be uncommon.

    unassignedRank %= (numNics - numAssignedNics);

    int count = 0;
    for (int nid = 0; nid < numNics; nid++) {
      if (nics[nid].assigned == false) {
        if (unassignedRank == count) {
          nic = nics[nid].obj;
          goto done;
        }
        count++;
      }
    }
  }

done:
  if (nic != NULL) {
    nicAttr = &(nic->attr->pcidev);
    if (outAddr != NULL) {
      outAddr->domain = nicAttr->domain;
      outAddr->bus = nicAttr->bus;
      outAddr->device = nicAttr->dev;
      outAddr->function = nicAttr->func;
      result = outAddr;
    }
  }
  if (nics != NULL) {
    sys_free(nics);
  }
  if (assignedNics != NULL) {
    sys_free(assignedNics);
  }
  return result;
}


static
void chk_err_fn(const char* file, int lineno, const char* what) {
  chpl_internal_error_v("%s: %d: !(%s)", file, lineno, what);
}


static
void chk_err_errno_fn(const char* file, int lineno, const char* what) {
  chpl_internal_error_v("%s: %d: !(%s): %s", file, lineno, what,
                        strerror(errno));
}
