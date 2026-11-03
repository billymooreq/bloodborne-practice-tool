"""Generates rva.h (RVAs of bb-probe.exe 1.9.5 runtime globals) from tools/bbprobe_195_symbols.txt.
Static names that occur more than once are picked by their neighbour in the same object file."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
SYMS = os.path.join(HERE, "..", "tools", "bbprobe_195_symbols.txt")
lines = [l.split(None, 3) for l in open(SYMS)]
def find(name, near=None):
    hits = [i for i, l in enumerate(lines) if l[3].strip() == name]
    if near:
        anchor = next(i for i, l in enumerate(lines) if l[3].strip() == near)
        hits.sort(key=lambda i: abs(i - anchor))
    if not hits: raise SystemExit("missing symbol " + name)
    return int(lines[hits[0]][0], 16)
WANT = [  # (macro, symbol, neighbour for disambiguation)
    ("RVA_VMA_COUNT", "vma_count", None),
    # host side of the GPU compute queues: last offset submitted per ring (sceGnmDingDong)
    ("RVA_ASC_NEXT_OFFS", "_ZN9Libraries9GnmDriverL16asc_next_offs_dwE", None),
    # guest clocks that do not start at the process start (task 5: a slot after a PC restart)
    ("RVA_READ_TSC", "read_tsc", None),
    ("RVA_KERNEL_CLOCK_GETTIME", "kernel_clock_gettime", None),
    ("RVA_POSIX_CLOCK_GETTIME", "posix_clock_gettime", None),
    ("RVA_VMAS", "vmas", None),
    ("RVA_BACKING_BASE", "backing_base", None),
    ("RVA_HOOK_INVALIDATE", "hook_invalidate", None),
    ("RVA_LOW_NEXT", "low_next", None),
    ("RVA_THREADS", "threads", "tls_memsz"),
    ("RVA_TLS_SLOT", "tls_slot", "tls_memsz"),
    ("RVA_LIVERPOOL", "liverpool", None),
    ("RVA_SUBMISSION_LOCK", "_ZN9Libraries9GnmDriverL15submission_lockE", None),
    ("RVA_FRAMES_SUBMITTED", "_ZN9Libraries9GnmDriverL16frames_submittedE", None),
    ("RVA_FN_SUBMITDONE", "_ZN9Libraries9GnmDriver16sceGnmSubmitDoneEv", None),
    ("RVA_FN_SUBMITANDFLIP", "_ZN9Libraries9GnmDriver33sceGnmSubmitAndFlipCommandBuffersEjPPjS1_S2_S1_jjjx", None),
    ("RVA_FN_VIDEOOUTSUBMITFLIP", "_ZN9Libraries8VideoOut21sceVideoOutSubmitFlipEiiix", None),
    # destroy/delete of host-side sync objects (kept alive while savestates exist)
    ("RVA_KEEP_MUTEX_DESTROY", "mutex_destroy", "mutex_init"),
    ("RVA_KEEP_POSIX_MUTEX_DESTROY", "posix_mutex_destroy", "mutex_init"),
    ("RVA_KEEP_COND_DESTROY", "cond_destroy", "mutex_init"),
    ("RVA_KEEP_POSIX_COND_DESTROY", "posix_cond_destroy", "mutex_init"),
    ("RVA_KEEP_MATTR_DESTROY", "attr_destroy", "mutex_init"),
    ("RVA_KEEP_POSIX_MATTR_DESTROY", "posix_attr_destroy", "mutex_init"),
    ("RVA_KEEP_RW_DESTROY", "rw_destroy", None),
    ("RVA_KEEP_SEM_DELETE", "sem_delete", None),
    ("RVA_KEEP_TATTR_DESTROY", "attr_destroy", "tls_memsz"),
    ("RVA_KEEP_POSIX_TATTR_DESTROY", "posix_attr_destroy", "tls_memsz"),
    # creation of the tracked objects (index order = destroy order above)
    ("RVA_INIT_MUTEX", "mutex_init", "mutex_destroy"),
    ("RVA_INIT_POSIX_MUTEX", "posix_mutex_init", "mutex_destroy"),
    ("RVA_INIT_COND", "cond_init", "mutex_destroy"),
    ("RVA_INIT_POSIX_COND", "posix_cond_init", "mutex_destroy"),
    ("RVA_INIT_MATTR", "attr_init", "mutex_destroy"),
    ("RVA_INIT_POSIX_MATTR", "posix_attr_init", "mutex_destroy"),
    # VideoOut port and event queues (host state the guest compares its own counters with)
    ("RVA_VO_DRIVER", "_ZN9Libraries8VideoOutL6driverE.0", None),
    # overlay (task 7): the port's "its menu captures input" answer (pad, mouse capture, cursor) and re-presenting the last frame
    ("RVA_CAPTURES_INPUT", "_ZN9BbOverlay13CapturesInputEv", None),
    ("RVA_PAD_CAPTURE", "bbgpu_overlay_captures_input", None),   # what pad_read_state asks
    ("RVA_DRAW_LAST_FRAME", "_ZN9Libraries8VideoOut14VideoOutDriver13DrawLastFrameEv", None),
    ("RVA_VO_GETPORT", "_ZN9Libraries8VideoOut14VideoOutDriver7GetPortEi", None),
    ("RVA_KQUEUES", "_ZN9Libraries6KernelL7kqueuesE", None),
    ("RVA_MUTEX_LOCK", "_ZNSt3__15mutex4lockEv", None),
    ("RVA_MUTEX_UNLOCK", "_ZNSt3__15mutex6unlockEv", None),
    ("RVA_CV_NOTIFY_ALL", "_ZNSt3__118condition_variable10notify_allEv", None),
    # guest file table (runtime_file.c) and its C helpers
    ("RVA_FILES", "files", None),
    ("RVA_USER_ROOT", "user_root", None),   # char[512]: the port's user folder (saves in <it>/savedata)
    ("RVA_FILE_OPEN", "runtime_file_open", None),
    ("RVA_FILE_CLOSE", "runtime_file_close", None),
    ("RVA_FILE_LSEEK", "runtime_file_lseek", None),
    # kernel semaphores (runtime_sema.c): registry list and the SysV sceKernelSignalSema
    ("RVA_SEMA_REGISTRY", "registry", "sem_signal"),
    ("RVA_SEM_SIGNAL", "sem_signal", None),
    # GPU readback: MemoryManager singleton (+8 = Rasterizer*), Rasterizer::ReadMemory(addr, size, assume_locks)
    ("RVA_MEMMGR_INSTANCE", "_ZN6Common9SingletonIN4Core13MemoryManagerEE10m_instanceE", None),
    ("RVA_RAST_READMEMORY", "_ZN6Vulkan10Rasterizer10ReadMemoryEyyb", None),
    # early mode (launch.py): import resolver, guest stack canary, process clock, original mutex functions (bench)
    ("RVA_RUNTIME_RESOLVE", "runtime_resolve", None),
    ("RVA_STACK_CANARY", "stack_canary", None),
    ("RVA_PROCESS_START", "process_start", None),
    ("RVA_PROCESS_TIME", "process_time", None),
    ("RVA_ORIG_MUTEX_LOCK", "mutex_lock", "mutex_init"),
    ("RVA_ORIG_MUTEX_UNLOCK", "mutex_unlock", "mutex_init"),
    # native TLS of bb-probe (per host thread): _tls_index, template start, runtime thread-locals
    ("RVA_TLS_INDEX", "_tls_index", None),
    ("RVA_TLS_START", "_tls_start", None),
    ("RVA_TLSV_KEY_VALUES", "key_values", None),
    ("RVA_TLSV_TLS_BLOCKS", "tls_blocks", None),
    ("RVA_TLSV_GUEST_ERRNO", "guest_errno", None),
    ("RVA_TLS_MODULES", "tls_modules", None),
    # canonical stack depth (task 6): the exe entry (main thread) and the runtime's start of every guest thread
    ("RVA_EXE_ENTRY", "WinMainCRTStartup", None),
    ("RVA_HOST_START", "host_start", None),
    # audio decoder (runtime_ajm.c, task 6): contexts[9] (Context*), batches[256], ATRAC9 handles (calloc 0x168c0 / free)
    ("RVA_AJM_CONTEXTS", "contexts", None),
    ("RVA_AJM_BATCHES", "batches", None),
    ("RVA_ATRAC9_GET_HANDLE", "Atrac9GetHandle", None),
    ("RVA_ATRAC9_RELEASE_HANDLE", "Atrac9ReleaseHandle", None),
    # task 6.1: host work still in flight at the consistent point (BbCopy host copy pool: function-local static)
    ("RVA_BBCOPY_POOL", "_ZZN6BbCopy12_GLOBAL__N_17GetPoolEvE4pool", None),
    ("RVA_BBCOPY_POOL_GUARD", "_ZGVZN6BbCopy12_GLOBAL__N_17GetPoolEvE4pool", None),
]
out = ["/* generated by gen_rva.py from bbprobe_195_symbols.txt: bb-probe.exe 1.9.5 */",
       '#define BBPROBE_195_SHA256 "71149dac2ca02e28e9bb451b2338e3cdfd47544be27fc306695c7393d74bfc0a"']
for macro, name, near in WANT:
    out.append("#define %-26s 0x%08xull /* %s */" % (macro, find(name, near), name))
out += ["/* AmdGpu::Liverpool fields, from the inlined IsGpuIdle in sceGnmSubmitDone (RVA 0x8b4e8) */",
        "#define LIVERPOOL_NUM_SUBMITS      0x39598",
        "#define LIVERPOOL_WORK_RETIRED     0x3959c",
        "/* task 6.1 (disassembly): Liverpool pending fences deque (NotePendingFences / PendingFenceValue), Rasterizer -> Scheduler",
        "   (RunEventWriteEop), Scheduler: master semaphore ticks, pending ops deque (PopPendingOperations), host copies and",
        "   deferred signals (WaitHostCopies / SignalAfterHostCopies / WaitDeferredSignals); BbCopy pool (AfterCopies / WaitAsync) */",
        "#define LIVERPOOL_PFENCE_SIZE      0x34128",
        "#define LIVERPOOL_RAST             0x39570",
        "#define RAST_SCHEDULER             0x1d403d8",
        "#define RAST_PENDING_WRITES        0x21894a0",
        "#define SCHED_TICK_DONE            0x38",
        "#define SCHED_TICK_CUR             0x40",
        "#define SCHED_PENDING_OPS          0x460",
        "#define SCHED_PENDING_OPS_N        0x494",
        "#define SCHED_HCOPY_ISSUED         0x6e0",
        "#define SCHED_HCOPY_DONE           0x6e8",
        "#define SCHED_DEFER_ISSUED         0x6f0",
        "#define SCHED_DEFER_DONE_PTR       0x6f8",
        "#define BBCOPY_TASKS               0x70",
        "#define BBCOPY_EPOCHS              0xa0",
        "#define BBCOPY_B0                  0xb0",
        "#define BBCOPY_B8                  0xb8",
        "/* guest image import slots (fixed addresses) */",
        "#define SLOT_SUBMITDONE            0x8053E5790ull",
        "#define SLOT_SUBMITANDFLIP         0x8053E5730ull",
        "#define SLOT_VIDEOOUTSUBMITFLIP    0x8053E5878ull",
        "/* GuestThread (runtime_thread.c, MinGW layout) */",
        "#define GT_TCB 0", "#define GT_TLS_BLOCK 8", "#define GT_NAME 104", "#define GT_FINISHED 140",
        "#define GT_HOST_OWNED 148", "#define GT_NEXT 408",
        "/* VideoOutPort (from sceVideoOutGetFlipStatus / GetBufferLabelAddress code): flip_status, vblank_status,",
        "   buffer_labels[16], port_mutex, prev_index; EqueueInternal (TriggerEvent): m_mutex, m_events, m_cond */",
        "#define VO_FLIP_STATUS 0x2F0", "#define VO_VBLANK_STATUS 0x330", "#define VO_LABELS 0x1B0",
        "#define VO_PORT_MUTEX 0x390", "#define VO_PREV_INDEX 0x3AC",
        "#define EQ_MUTEX 0x20", "#define EQ_EVENTS 0x28", "#define EQ_COND 0x40", "#define EQ_EVENT_SIZE 0x48", "#define EQ_EV_TRIGGERED 0x40",
        "/* File (runtime_file.c, MinGW layout): used, host, dirty, dir*, position, path[512], commit*, temp*; 1024 entries */",
        "#define FILE_SIZE 560", "#define FILE_MAX 1024", "#define FILE_DIR 16", "#define FILE_PATH 32", "#define FILE_COMMIT 544", "#define FILE_TEMP 552",
        "/* Sema (runtime_sema.c): u32 id, attr; s32 count, initial, maximum; u32 active; int deleted; Waiter *first; Sema *next */",
        "#define SEMA_COUNT 8", "#define SEMA_ACTIVE 20", "#define SEMA_NEXT 40"]
open(os.path.join(HERE, "rva.h"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
