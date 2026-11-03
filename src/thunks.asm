; Hooks for the guest's import slots (sceGnmSubmitDone, sceGnmSubmitAndFlipCommandBuffers,
; sceVideoOutSubmitFlip). Guest code calls them with the System V ABI; bbs_pre is Win64.
; The thunk saves every System V argument register (rdi rsi rdx rcx r8 r9 rax xmm0-7) and the
; System V callee-saved registers (rbx rbp r12-r15) on the stack, calls bbs_pre(kind, frame) and
; then jumps to the original function with the registers and stack exactly as the guest left them.
; frame = lowest address of the saved area: L2 restores the main thread's stack from there up, so
; the registers saved here come back with it.
; Win64 bbs_pre itself preserves rbx rbp rdi rsi r12-r15 xmm6-15 (a superset of System V's).

EXTERN bbs_pre:PROC
EXTERN bbs_orig:QWORD

.code

THUNK MACRO idx
bbs_hook&idx PROC
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push rax                    ; 13 pushes: entry rsp = 8 mod 16, now 0 mod 16
    sub rsp, 160                ; 32 shadow + 8 * 16 xmm
    movdqu [rsp+32], xmm0
    movdqu [rsp+48], xmm1
    movdqu [rsp+64], xmm2
    movdqu [rsp+80], xmm3
    movdqu [rsp+96], xmm4
    movdqu [rsp+112], xmm5
    movdqu [rsp+128], xmm6
    movdqu [rsp+144], xmm7
    cld
    mov ecx, idx
    lea rdx, [rsp+32]
    call bbs_pre
    movdqu xmm0, [rsp+32]
    movdqu xmm1, [rsp+48]
    movdqu xmm2, [rsp+64]
    movdqu xmm3, [rsp+80]
    movdqu xmm4, [rsp+96]
    movdqu xmm5, [rsp+112]
    movdqu xmm6, [rsp+128]
    movdqu xmm7, [rsp+144]
    add rsp, 160
    pop rax
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    jmp qword ptr [bbs_orig + idx*8]
bbs_hook&idx ENDP
ENDM

THUNK 0
THUNK 1
THUNK 2

; System V entry -> Win64 C function with one argument (the replacements for destroy/delete).
; rdi/rsi are callee-saved in Win64, so the C function keeps them; entry rsp = 8 mod 16.
ADAPT MACRO name
EXTERN name&_c:PROC
name PROC
    sub rsp, 40
    cld
    mov rcx, rdi
    call name&_c
    add rsp, 40
    ret
name ENDP
ENDM

; Tracked objects (mutex, cond, mutex attr; Orbis and POSIX variants), idx 0..5.
; init: call the original with the guest's registers untouched, then keep_on_init(result, out, idx)
; records the new handle with the current snapshot generation.
EXTERN keep_on_init:PROC
EXTERN keep_on_destroy:PROC
EXTERN keep_iorig:QWORD
EXTERN keep_dorig:QWORD

TRACK_INIT MACRO idx
track_init&idx PROC
    push rdi                    ; out pointer (first argument); rsp = 0 mod 16
    sub rsp, 16
    call qword ptr [keep_iorig + idx*8]
    mov dword ptr [rsp], eax
    mov ecx, eax
    mov rdx, [rsp+16]
    mov r8d, idx
    sub rsp, 32
    cld
    call keep_on_init
    add rsp, 32
    mov eax, dword ptr [rsp]
    add rsp, 16
    pop rdi
    ret
track_init&idx ENDP
ENDM

; destroy: keep_on_destroy(handle_ptr, idx) returns bit 32 set = let the original free it,
; otherwise the object is kept and eax is the return code.
TRACK_DESTROY MACRO idx
track_destroy&idx PROC
    push rdi                    ; rsp = 0 mod 16
    sub rsp, 32
    cld
    mov rcx, rdi
    mov edx, idx
    call keep_on_destroy
    add rsp, 32
    pop rdi
    bt rax, 32
    jc forward&idx
    ret
forward&idx:
    jmp qword ptr [keep_dorig + idx*8]
track_destroy&idx ENDP
ENDM

TRACK_INIT 0
TRACK_INIT 1
TRACK_INIT 2
TRACK_INIT 3
TRACK_INIT 4
TRACK_INIT 5
TRACK_DESTROY 0
TRACK_DESTROY 1
TRACK_DESTROY 2
TRACK_DESTROY 3
TRACK_DESTROY 4
TRACK_DESTROY 5

ADAPT keep_rw_destroy
ADAPT keep_sem_delete
ADAPT keep_tattr_destroy
ADAPT keep_posix_tattr_destroy

; int64 sysv_call2(fn, a, b): calls a System V function from Win64 code. rdi/rsi and xmm6-15 are
; callee-saved in Win64 but not in System V.
sysv_call2 PROC
    push rdi
    push rsi
    sub rsp, 168                ; 10 xmm + alignment: entry 8 mod 16, +16 pushes, +168 -> 0 mod 16
    movdqu [rsp], xmm6
    movdqu [rsp+16], xmm7
    movdqu [rsp+32], xmm8
    movdqu [rsp+48], xmm9
    movdqu [rsp+64], xmm10
    movdqu [rsp+80], xmm11
    movdqu [rsp+96], xmm12
    movdqu [rsp+112], xmm13
    movdqu [rsp+128], xmm14
    movdqu [rsp+144], xmm15
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    call rax
    movdqu xmm6, [rsp]
    movdqu xmm7, [rsp+16]
    movdqu xmm8, [rsp+32]
    movdqu xmm9, [rsp+48]
    movdqu xmm10, [rsp+64]
    movdqu xmm11, [rsp+80]
    movdqu xmm12, [rsp+96]
    movdqu xmm13, [rsp+112]
    movdqu xmm14, [rsp+128]
    movdqu xmm15, [rsp+144]
    add rsp, 168
    pop rsi
    pop rdi
    ret
sysv_call2 ENDP

; int64 sysv_call7(fn, a, b, c, d, e, f, g): the same with seven arguments (the seventh on the stack).
; Win64 passes d..g at [rsp+40..64] on entry: after the pushes and the frame they are at +0F0h..+108h.
sysv_call7 PROC
    push rdi
    push rsi
    sub rsp, 184                ; entry 8 mod 16, +16 pushes, +184 -> 0 mod 16; [rsp] = the seventh argument
    movdqu [rsp+16], xmm6
    movdqu [rsp+32], xmm7
    movdqu [rsp+48], xmm8
    movdqu [rsp+64], xmm9
    movdqu [rsp+80], xmm10
    movdqu [rsp+96], xmm11
    movdqu [rsp+112], xmm12
    movdqu [rsp+128], xmm13
    movdqu [rsp+144], xmm14
    movdqu [rsp+160], xmm15
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov rcx, [rsp+0F0h]
    mov r8, [rsp+0F8h]
    mov r9, [rsp+100h]
    mov r10, [rsp+108h]
    mov [rsp], r10
    call rax
    movdqu xmm6, [rsp+16]
    movdqu xmm7, [rsp+32]
    movdqu xmm8, [rsp+48]
    movdqu xmm9, [rsp+64]
    movdqu xmm10, [rsp+80]
    movdqu xmm11, [rsp+96]
    movdqu xmm12, [rsp+112]
    movdqu xmm13, [rsp+128]
    movdqu xmm14, [rsp+144]
    movdqu xmm15, [rsp+160]
    add rsp, 184
    pop rsi
    pop rdi
    ret
sysv_call7 ENDP


; vsync (early mode): System V entry -> Win64 C function with up to 4 arguments. Win64 keeps rbx rbp
; rdi rsi r12-r15 xmm6-15 (a superset of what System V callers expect to survive).
SV MACRO name
EXTERN name&_c:PROC
PUBLIC name
name PROC
    sub rsp, 40
    cld
    mov r9, rcx
    mov r8, rdx
    mov rdx, rsi
    mov rcx, rdi
    call name&_c
    add rsp, 40
    ret
name ENDP
ENDM
SV vs_mattr_init
SV vs_mattr_type
SV vs_mattr_destroy
SV vs_mattr_protocol
SV vs_mutex_init
SV vs_mutex_lock
SV vs_mutex_trylock
SV vs_mutex_timedlock
SV vs_mutex_unlock
SV vs_mutex_destroy
SV vs_cond_init
SV vs_cond_destroy
SV vs_cond_wait
SV vs_cond_timedwait
SV vs_cond_signal
SV vs_cond_broadcast
SV vs_rw_init
SV vs_rw_destroy
SV vs_rw_read
SV vs_rw_write
SV vs_rw_tryread
SV vs_rw_trywrite
SV vs_rw_unlock
SV vs_rw_timedread
SV vs_rw_timedwrite
SV vs_pattr_init
SV vs_pattr_type
SV vs_pattr_destroy
SV vs_pmutex_init
SV vs_pmutex_destroy
SV vs_pmutex_lock
SV vs_pmutex_trylock
SV vs_pmutex_unlock
SV vs_pcond_init
SV vs_pcond_destroy
SV vs_pcond_wait
SV vs_pcond_timedwait
SV vs_pcond_signal
SV vs_pcond_broadcast
SV vc_tsc
SV vc_kclock
SV vc_pclock
SV vs_sema_wait
SV vs_sema_poll
SV vs_sema_signal
SV vs_sema_cancel
SV vs_sema_delete

; Canonical stack depth (early mode). Windows starts every thread's stack pointer a random distance
; (multiples of 16 bytes) below the top of its stack: the same game frames then sit at another distance
; from the top in every run, and a cross-process load shifted stack addresses by amounts that change the
; low bits (a flag byte stored over a stale stack address came out set). The exe's entry point and the
; runtime's guest thread start (host_start) are entered through these with the stack pointer at a fixed
; distance below the top (TEB StackBase), never above where it already is. Frame unwind data: rbp.
EXTERN canon_main_depth:QWORD
EXTERN canon_thread_depth:QWORD
EXTERN canon_entry_tramp:QWORD
EXTERN canon_start_tramp:QWORD
EXTERN canon_done:DWORD
EXTERN canon_missed:DWORD
; Task 6.1: fixed stack addresses. fs_enter (bbstate.c) gives the thread a stack at a fixed address (by its
; name) and sets the TEB stack limits: the returned stack pointer (0 = keep this stack, canonical depth
; only). After the start function returns (a guest thread's exit longjmps back into host_start, which
; returns), fs_leave runs on the thread's own stack again: TEB limits back, the fixed stack released.
EXTERN fs_enter:PROC
EXTERN fs_leave:PROC
CANON MACRO name, depth, tramp
PUBLIC name
name PROC FRAME
    push rbp
    .pushreg rbp
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog
    mov rbx, rcx                ; the start function's argument (host_start: GuestThread*)
    sub rsp, 32
    call fs_enter
    test rax, rax
    jz name&_depth
    mov rsp, rax
    lock inc canon_done
    jmp name&_go
name&_depth:
    mov rax, gs:[8]
    sub rax, depth
    cmp rax, rsp
    jae name&_missed
    mov rsp, rax
    lock inc canon_done
    jmp name&_go
name&_missed:
    lock inc canon_missed
name&_go:
    mov rcx, rbx
    sub rsp, 32
    call qword ptr [tramp]
    mov rsi, rax
    lea rsp, [rbp - 32]
    call fs_leave
    mov rax, rsi
    mov rsp, rbp
    pop rsi
    pop rbx
    pop rbp
    ret
name ENDP
ENDM
CANON canon_entry, canon_main_depth, canon_entry_tramp
CANON canon_start, canon_thread_depth, canon_start_tramp

; sceKernelCreateSema(out, name, attr, initial, maximum, options): the last three go to the C function as a block
EXTERN vs_sema_create_c:PROC
PUBLIC vs_sema_create
vs_sema_create PROC
    sub rsp, 72                 ; 32 shadow + 24 block + 16 (entry 8 mod 16 -> 0 mod 16)
    cld
    movsxd rax, ecx
    mov [rsp+32], rax           ; initial
    movsxd rax, r8d
    mov [rsp+40], rax           ; maximum
    mov [rsp+48], r9            ; options
    lea r9, [rsp+32]
    mov r8d, edx
    mov rdx, rsi
    mov rcx, rdi
    call vs_sema_create_c
    add rsp, 72
    ret
vs_sema_create ENDP

END
