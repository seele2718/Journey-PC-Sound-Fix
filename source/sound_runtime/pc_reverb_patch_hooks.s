/* Localized Windows-x64 wrappers for the static Journey PC reverb repair.
 *
 * The patched sites remain five-byte near CALLs (six/eight-byte sites receive
 * trailing NOPs). Wrappers preserve the displaced instruction's contract;
 * the ReverbBarn epilogue wrapper reproduces its register load before return.
 *
 * Address legend:
 *   0x140...      preferred virtual address in Journey.exe
 *   [register+N]  byte offset in the live object or stack frame at that hook
 *   0x140553...   Journey's import-address-table slots, dereferenced before use
 * The absolute targets are accepted only after the Python patcher validates
 * their original bytes/import identities against the input executable.
 */

        .text

        .macro absolute_tail name, address
        .p2align 4
        .globl \name
        .def \name; .scl 2; .type 32; .endef
\name:
        movabsq $\address, %r11
        jmpq *%r11
        .endm

        /* Surviving original helpers reused by the repair. */
        absolute_tail journey_original_tone_create,    0x1402c0310
        absolute_tail journey_original_stream_create,  0x1402c4ec0
        absolute_tail journey_original_ducker_fade,    0x1402c0770

        /* This is FMOD_System_PlaySound through Journey's verified IAT slot. */
        .p2align 4
journey_original_play_sound:
        movabsq $0x140553f00, %r11
        jmpq *(%r11)

        .p2align 4
        .globl journey_original_system_update
        .def journey_original_system_update; .scl 2; .type 32; .endef
journey_original_system_update:
        movabsq $0x140553f48, %r11
        jmpq *(%r11)

        .p2align 4
        .globl journey_ducker_category_group
        .def journey_ducker_category_group; .scl 2; .type 32; .endef
journey_ducker_category_group:
        /* Audio wrapper global -> FMOD System at +0 -> category group table.
         * RCX is the zero-based authored sound category. */
        movabsq $0x143cdd9e0, %rax
        movq (%rax), %rax
        movq 0x48(%rax,%rcx,8), %rax
        ret

        .p2align 4
        .globl journey_hook_audio_system_post_init
        .set journey_hook_audio_system_post_init,journey_event_startup_release

        .p2align 4
        .globl journey_hook_system_update
        .def journey_hook_system_update; .scl 2; .type 32; .endef
        .seh_proc journey_hook_system_update
journey_hook_system_update:
        .seh_endprologue
        jmp journey_reverb_patch_update_or_original
        .seh_endproc

        .p2align 4
        .globl journey_hook_reverb_barn_post_update
        .def journey_hook_reverb_barn_post_update; .scl 2; .type 32; .endef
        .seh_proc journey_hook_reverb_barn_post_update
journey_hook_reverb_barn_post_update:
        subq $0x28, %rsp
        .seh_stackalloc 0x28
        .seh_endprologue
        movq %rbx, %rcx
        call journey_reverb_patch_publish
        /* Original RSP +0x158 is wrapper-entry RSP +0x160. */
        movq 0x188(%rsp), %rbx
        addq $0x28, %rsp
        ret
        .seh_endproc

        /* R10 holds the raw v12 Tone descriptor here, before two native calls
         * are allowed to clobber that volatile register.  RBP+0x48 is unused
         * by this switch case (its only use belongs to a later, disjoint
         * case), so it safely carries the descriptor to the create hook. */
        .p2align 4
        .globl journey_hook_tone_descriptor_capture
        .def journey_hook_tone_descriptor_capture; .scl 2; .type 32; .endef
journey_hook_tone_descriptor_capture:
        movq %r10, 0x48(%rbp)
        movzwl 0x12(%r10), %edx
        ret

        .p2align 4
        .globl journey_hook_tone_channel_create
        .set journey_hook_tone_channel_create,journey_event_tone_create

        .p2align 4
        .globl journey_hook_stream_main_create
        .set journey_hook_stream_main_create,journey_event_stream_main_create

        .p2align 4
        .globl journey_hook_stream_predecessor_channel_create
        .set journey_hook_stream_predecessor_channel_create,journey_event_stream_predecessor_create

        .p2align 4
        .globl journey_hook_stream_successor_create
        .set journey_hook_stream_successor_create,journey_event_stream_successor_create

        /* Positive opcode 10.  If the new adapter is unavailable or rejects
         * the row, restore every stock helper argument and tail-call it. */
        .p2align 4
        .globl journey_hook_ducker_configure
        .def journey_hook_ducker_configure; .scl 2; .type 32; .endef
        .seh_proc journey_hook_ducker_configure
journey_hook_ducker_configure:
        subq $0x48, %rsp
        .seh_stackalloc 0x48
        .seh_endprologue
        movq %rcx, 0x20(%rsp)
        movq %rdx, 0x28(%rsp)
        movss %xmm1, 0x30(%rsp)
        movss %xmm2, 0x34(%rsp)
        movss %xmm3, 0x38(%rsp)
        movq %r14, %rcx
        call journey_ducker_patch_configure
        testl %eax, %eax
        je 1f
        addq $0x48, %rsp
        ret
1:
        movq 0x20(%rsp), %rcx
        movq 0x28(%rsp), %rdx
        movss 0x30(%rsp), %xmm1
        movss 0x34(%rsp), %xmm2
        movss 0x38(%rsp), %xmm3
        addq $0x48, %rsp
        jmp journey_original_ducker_fade
        .seh_endproc

        .p2align 4
        .globl journey_hook_ducker_retire
        .def journey_hook_ducker_retire; .scl 2; .type 32; .endef
        .seh_proc journey_hook_ducker_retire
journey_hook_ducker_retire:
        subq $0x48, %rsp
        .seh_stackalloc 0x48
        .seh_endprologue
        movq %rcx, 0x20(%rsp)
        movq %rdx, 0x28(%rsp)
        movss %xmm1, 0x30(%rsp)
        movss %xmm2, 0x34(%rsp)
        movss %xmm3, 0x38(%rsp)
        movq %r14, %rcx
        call journey_ducker_patch_retire
        testl %eax, %eax
        je 2f
        addq $0x48, %rsp
        ret
2:
        movq 0x20(%rsp), %rcx
        movq 0x28(%rsp), %rdx
        movss 0x30(%rsp), %xmm1
        movss 0x34(%rsp), %xmm2
        movss 0x38(%rsp), %xmm3
        addq $0x48, %rsp
        jmp journey_original_ducker_fade
        .seh_endproc

        .p2align 4
        .globl journey_hook_ducker_reset_forget
        .def journey_hook_ducker_reset_forget; .scl 2; .type 32; .endef
        .seh_proc journey_hook_ducker_reset_forget
journey_hook_ducker_reset_forget:
        subq $0x48, %rsp
        .seh_stackalloc 0x48
        .seh_endprologue
        movq %rcx, 0x20(%rsp)
        movss %xmm1, 0x30(%rsp)
        movss %xmm2, 0x34(%rsp)
        movss %xmm3, 0x38(%rsp)
        movq %rdi, %rcx
        movl %r14d, %edx
        call journey_ducker_patch_forget
        testl %eax, %eax
        je 3f
        addq $0x48, %rsp
        ret
3:
        movq 0x20(%rsp), %rcx
        movss 0x30(%rsp), %xmm1
        movss 0x34(%rsp), %xmm2
        movss 0x38(%rsp), %xmm3
        addq $0x48, %rsp
        jmp journey_original_ducker_fade
        .seh_endproc

/* Startup boundary: original ReleaseSemaphore call1402be21b.
 * FMOD, manager and both group loops are ready, but worker/update remain behind
 * the existing native semaphore. Initialize and seed BEFORE releasing it.
 * RCX=semaphore, EDX=1, R8=previous-count pointer; RBX=audio wrapper. Forward
 * these untouched. The following R12-load at1402be221 remains original;
 * initialize once, before forwarding the native release.
 * No new lock/wait. Original import bridge is140553260, supplied by builder.
 */
        .text
        .p2align 4
        .globl journey_event_startup_release
        .seh_proc journey_event_startup_release
journey_event_startup_release:
        subq $0x48,%rsp
        .seh_stackalloc 0x48
        .seh_endprologue
        movq %rcx,0x20(%rsp)
        movq %rdx,0x28(%rsp)
        movq %r8,0x30(%rsp)
        movq %r9,0x38(%rsp)
        movq (%rbx),%rcx
        call journey_reverb_patch_initialize
        movq %rbx,%rcx
        movabsq $0x140686d40,%rdx
        call journey_event_owner_startup
        movq 0x20(%rsp),%rcx
        movq 0x28(%rsp),%rdx
        movq 0x30(%rsp),%r8
        movq 0x38(%rsp),%r9
        addq $0x48,%rsp
        jmp journey_event_original_semaphore_release
        .seh_endproc

/* Native cue admission wrapper at VA 0x1402c3158.
 * Native Group::SetPaused receives RCX=cue group, EDX=0; RBX is cue.
 * Start already holds+1 activity reference, inside serialized cue worker.
 * Do not use at arbitrary activation entry (main-frame producers differ).
 * Common root/child activation sets flag6; worker clears it after this call.
 * Admit ONCE per native activation, not on every interpreted grain. Cues
 * canceled before first update take the stop path and are never admitted here.
 * Observer validates native pool identity and idempotently admits this use;
 * it does not alter native scheduling/reference counts. Any special/preloaded
 * Stream remains inaudible under this paused cue group until this boundary.
 * Tail forwarding keeps the original stack arguments and native return value.
 */
        .text
        .p2align 4
        .globl journey_event_cue_unpause
        .seh_proc journey_event_cue_unpause
journey_event_cue_unpause:
        subq $0x88,%rsp
        .seh_stackalloc 0x88
        .seh_endprologue
        movq %rcx,0x20(%rsp)
        movq %rdx,0x28(%rsp)
        movq %r8,0x30(%rsp)
        movq %r9,0x38(%rsp)
        movdqu %xmm0,0x40(%rsp)
        movdqu %xmm1,0x50(%rsp)
        movdqu %xmm2,0x60(%rsp)
        movdqu %xmm3,0x70(%rsp)
        movq %rbx,%rcx
        call journey_event_cue_admit_native
        movq 0x20(%rsp),%rcx
        movq 0x28(%rsp),%rdx
        movq 0x30(%rsp),%r8
        movq 0x38(%rsp),%r9
        movdqu 0x40(%rsp),%xmm0
        movdqu 0x50(%rsp),%xmm1
        movdqu 0x60(%rsp),%xmm2
        movdqu 0x70(%rsp),%xmm3
        addq $0x88,%rsp
        jmp journey_event_original_group_pause
        .seh_endproc

/* Native Tone creation and pre-unpause adapters.
 *
 * Original-image contract: 1402c0310 pushes four registers and allocates
 * 0xa8 bytes, total 0xc8 below entry RSP. Its own shadow-space writes end at
 * entry+0x20. The wrapper owns the added fifth/sixth stack arguments.
 *
 * IMPORTANT: RSI is NOT a reliable cue at the final unpause. The native
 * envelope branch converts it to a DSP-clock timestamp at 1402c05a9.
 * Carry both cue and descriptor explicitly, without global/TLS scratch.
 */
        .text
        .p2align 4
        .globl journey_event_tone_create
        .seh_proc journey_event_tone_create
journey_event_tone_create:
        subq $0x38, %rsp
        .seh_stackalloc 0x38
        .seh_endprologue
        movq %rcx, 0x20(%rsp)       /* added arg5: native cue */
        movq 0x48(%rbp), %rax      /* existing case-local descriptor capture */
        movq %rax, 0x28(%rsp)      /* added arg6: selected Tone descriptor */
        call journey_event_original_tone_create
journey_event_tone_return:
        /* Preserve the creator's Channel/NULL return. No late attachment. */
        addq $0x38, %rsp
        ret
        .seh_endproc

        .p2align 4
        .globl journey_event_tone_pre_unpause
        .seh_proc journey_event_tone_pre_unpause
journey_event_tone_pre_unpause:
        subq $0x38, %rsp
        .seh_stackalloc 0x38
        .seh_endprologue
        movq %rcx, 0x20(%rsp)      /* native ChannelControl receiver */
        movl %edx, 0x28(%rsp)      /* native pause argument, even on fallback */
        leaq journey_event_tone_return(%rip), %rax
        /* hook entry +8 = creator local RSP; +0xc8 = creator entry RSP.
         * After our 0x38 allocation, creator return address is at +0x108.
         * Only our wrapper supplies the additional arguments: guard FIRST.
         */
        cmpq %rax, 0x108(%rsp)
        jne 1f
        movq 0x130(%rsp), %rcx     /* creator entry +0x28: cue */
        movq 0x138(%rsp), %r8      /* creator entry +0x30: descriptor */
        testq %rcx, %rcx
        je 1f
        testq %r8, %r8
        je 1f
        movq 0x20(%rsp), %rdx
        testq %rdx, %rdx
        je 1f
        call journey_event_attach_tone
1:
        movq 0x20(%rsp), %rcx
        movl 0x28(%rsp), %edx
        addq $0x38, %rsp
        /* Tail-call the original import exactly once, including fallback.
         * It returns directly to the original creator after its unpause call.
         */
        jmp journey_event_original_set_paused
        .seh_endproc

/* Stream publication: carry caller context through the native pool
 * factory, without TLS/global scratch and without bypassing Stream+48 group.
 * Supported factory 1402c4ec0 frame=0x78; its virtual play
 * at1402c4fd8 reaches1402c4930 (frame=0x48), directly or through the frameless
 * 1402c4b10 tail wrapper. Hook1402c4a49 is the final Channel unpause.
 * Verified vtable +8 targets: 140686fc0,140686fe8,140687010.
 * Do not use these stack offsets for another factory/prologue.
 */
        .text
        .macro factory name, successor
        .p2align 4
        .globl \name
        .seh_proc \name
\name:
        subq $0xb8, %rsp
        .seh_stackalloc 0xb8
        .seh_endprologue
        movq 0xe0(%rsp), %rax
        movq %rax, 0x20(%rsp) /* original arg5 filename */
        movq %rcx, 0x40(%rsp)
        movq %rdx, 0x48(%rsp)
        movaps %xmm2, 0x50(%rsp)
        movq %r9, 0x60(%rsp)
        /* Prepare before native pool allocation; origin may be reused.
         * Native args are restored after this ordinary C call. */
        .if \successor
        movq 0x18(%rbp), %rcx
        movq 0x08(%rbp), %rdx
        .else
        movq %r14, %rcx
        movq -0x50(%rbp), %rdx
        .endif
        movl $\successor, %r8d
        leaq 0x70(%rsp), %r9
        call journey_event_prepare_stream
        leaq 0x70(%rsp), %rax
        movq %rax, 0x28(%rsp) /* extra arg6: stack-owned scalar snapshot */
        movq 0x40(%rsp), %rcx
        movq 0x48(%rsp), %rdx
        movaps 0x50(%rsp), %xmm2
        movq 0x60(%rsp), %r9
        call journey_event_original_stream_create
\name\()_return:
        addq $0xb8, %rsp
        ret
        .seh_endproc
        .endm
        factory journey_event_stream_main_create, 0
        factory journey_event_stream_successor_create, 1

        .p2align 4
        .globl journey_event_stream_pre_unpause
        .seh_proc journey_event_stream_pre_unpause
journey_event_stream_pre_unpause:
        subq $0x38, %rsp
        .seh_stackalloc 0x38
        .seh_endprologue
        movq %rcx, 0x20(%rsp)
        movl %edx, 0x28(%rsp)
        /* At hook entry: +8=play locals, +0x50=play entry,
         * +0x58=factory locals, +0xd0=factory entry/return address.
         * Add our0x38 frame => return at0x108. Guard before reading extras. */
        leaq journey_event_stream_main_create_return(%rip), %rax
        cmpq %rax, 0x108(%rsp)
        je 1f
        leaq journey_event_stream_successor_create_return(%rip), %rax
        cmpq %rax, 0x108(%rsp)
        jne 2f
1:
        movq 0x138(%rsp), %rdx /* factory arg6 snapshot */
        testq %rdx, %rdx
        je 2f
        cmpq $0, 0x20(%rsp)
        je 2f
        movq %rdi, %rcx       /* native playback's actual Stream owner */
        call journey_event_publish_stream
2:
        movq 0x20(%rsp), %rcx
        movl 0x28(%rsp), %edx
        addq $0x38, %rsp
        jmp journey_event_original_set_paused
        .seh_endproc

/* Original-image call site1402c42fc: callback-free, paused predecessor channel.
 * RBP is the transition object (+8 origin Stream,+18 cue). Preserve all five
 * PlaySound arguments and its result; following native seek/delay/fades and
 * Channel unpause at1402c4350 are left untouched. No global scratch. */
        .text
        .globl journey_event_stream_predecessor_create
        .seh_proc journey_event_stream_predecessor_create
journey_event_stream_predecessor_create:
        subq $0xa8, %rsp
        .seh_stackalloc 0xa8
        .seh_endprologue
        movq 0xd0(%rsp), %rax
        movq %rax, 0x20(%rsp)
        movq %rax, 0x28(%rsp)
        movq %rcx, 0x30(%rsp)
        movq %rdx, 0x38(%rsp)
        movq %r8, 0x40(%rsp)
        movl %r9d, 0x48(%rsp)
        movq 0x18(%rbp), %rcx
        movq 0x08(%rbp), %rdx
        movl $1, %r8d
        leaq 0x60(%rsp), %r9
        call journey_event_prepare_stream
        movq 0x30(%rsp), %rcx
        movq 0x38(%rsp), %rdx
        movq 0x40(%rsp), %r8
        movl 0x48(%rsp), %r9d
        call journey_event_original_play_sound
        movl %eax, 0x50(%rsp)
        testl %eax, %eax
        jne 1f
        movq 0x28(%rsp), %rax
        testq %rax, %rax
        je 1f
        movq (%rax), %rcx
        testq %rcx, %rcx
        je 1f
        leaq 0x60(%rsp), %rdx
        call journey_event_publish_predecessor
1:
        movl 0x50(%rsp), %eax
        addq $0xa8, %rsp
        ret
        .seh_endproc

/* Win64 native-lifecycle callback adapters.
 *
 * Replace callback addresses at their registration sites, not native callback
 * bodies. Preserve Channel userdata, all five callback arguments, all native
 * event types and the native return value. Notify before chaining: native END
 * may recycle a Stream/cue. The notifier must only retire a bound generation;
 * it must not mutate the FMOD graph, free callback state, or call native END.
 * Generation lookup uses the atomic native-channel binding registry.
 */
        .text
        .macro native_callback name, original
        .p2align 4
        .globl \name
        .seh_proc \name
\name:
        subq $0x48, %rsp
        .seh_stackalloc 0x48
        .seh_endprologue
        movq %rcx, 0x20(%rsp)
        movl %edx, 0x28(%rsp)
        movl %r8d, 0x30(%rsp)
        movq %r9, 0x38(%rsp)
        /* ChannelControl type CHANNEL=0, callback END=0. Unknown and
         * virtualization events must reach the original handler unchanged. */
        testl %edx, %edx
        jne 1f
        testl %r8d, %r8d
        jne 1f
        call journey_event_source_end
1:
        movq 0x20(%rsp), %rcx
        movl 0x28(%rsp), %edx
        movl 0x30(%rsp), %r8d
        movq 0x38(%rsp), %r9
        addq $0x48, %rsp
        /* Arg5 remains in the caller's original stack slot. Tail chaining
         * preserves it without copying/guessing a pointee or return value. */
        jmp \original
        .seh_endproc
        .endm

        native_callback journey_event_tone_callback, journey_event_original_tone_callback
        native_callback journey_event_stream_callback, journey_event_original_stream_callback

        /* The transition predecessor has no native callback. This adapter
         * is ONLY for that audited path; never replace an existing handler. */
        .p2align 4
        .globl journey_event_predecessor_callback
        .seh_proc journey_event_predecessor_callback
journey_event_predecessor_callback:
        subq $0x28, %rsp
        .seh_stackalloc 0x28
        .seh_endprologue
        testl %edx, %edx
        jne 2f
        testl %r8d, %r8d
        jne 2f
        call journey_event_source_end
2:
        xorl %eax, %eax
        addq $0x28, %rsp
        ret
        .seh_endproc

        /* Native call-site 1402bf011, AFTER cue flag5 early-exit guard:
         * RCX=group, RDI=cue. Do not attach this to finalizer entry. */
        .p2align 4
        .globl journey_event_cue_stop
        .seh_proc journey_event_cue_stop
journey_event_cue_stop:
        subq $0x28, %rsp
        .seh_stackalloc 0x28
        .seh_endprologue
        movq %rcx, 0x20(%rsp)
        movq %rcx, %rdx
        movq %rdi, %rcx
        call journey_event_cue_retire
        movq 0x20(%rsp), %rcx
        addq $0x28, %rsp
        jmp journey_event_original_group_stop
        .seh_endproc

/* Exact original-image bridges. IAT identities are validated by the builder. */
        absolute_tail journey_event_original_tone_create, 0x1402c0310
        absolute_tail journey_event_original_stream_create, 0x1402c4ec0
        absolute_tail journey_event_original_tone_callback, 0x1402bf320
        absolute_tail journey_event_original_stream_callback, 0x1402c4570
        .macro event_import name,address
        .globl \name
\name:
        movabsq $\address,%r11
        jmpq *(%r11)
        .endm
        event_import journey_event_original_semaphore_release,0x140553260
        event_import journey_event_original_group_pause,0x140553f10
        event_import journey_event_original_set_paused,0x140553f10
        event_import journey_event_original_group_stop,0x140554020
        event_import journey_event_original_play_sound,0x140553f00
        .globl journey_event_attach_tone
journey_event_attach_tone:
        jmp journey_reverb_patch_attach_tone

/* Change only the registered callback address; native userdata is untouched.
 * A future caller that supplies another callback is forwarded unchanged. */
        .macro register_callback name,original,replacement
        .globl \name
\name:
        movabsq $\original,%r11
        cmpq %r11,%rdx
        jne 1f
        leaq \replacement(%rip),%rdx
1:      movabsq $0x140553e98,%r11
        jmpq *(%r11)
        .endm
        register_callback journey_event_register_tone_callback,0x1402bf320,journey_event_tone_callback
        register_callback journey_event_register_stream_callback,0x1402c4570,journey_event_stream_callback
        .globl journey_event_install_predecessor_end
journey_event_install_predecessor_end:
        leaq journey_event_predecessor_callback(%rip),%rdx
        movabsq $0x140553e98,%r11
        jmpq *(%r11)
