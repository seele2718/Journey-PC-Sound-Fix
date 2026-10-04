/* Tone-cache repair for embedded bank media.
 *
 * REGION_RVA is the RVA where this appended code will live. Expressions such
 * as "blob_start + 0x2c8641 - REGION_RVA" therefore branch to an existing
 * Journey RVA without hard-coding the process load address. The extracted
 * .text must have no unresolved COFF relocations.
 *
 * The cache identity is (uint32 frequency << 32) | uint32 media_offset:
 *   Tone descriptor +0x44 = signed payload-relative media offset
 *   Tone descriptor +0x4c = requested frequency, converted to integer
 * The bank-owned tree node is left/parent/right at +0/+8/+0x10, color/isnil
 * at +0x18/+0x19, key at +0x20, and SoundRecord pointer at +0x28.
 */
.intel_syntax noprefix
.text
.global blob_start, first_search, creator_return, playback_search, creator_alloc_guard
.global reserve_vector, reserve_vector_end, loader_fragments_end, playback_end
.global creator_alloc_end, blob_end
blob_start:
/* Original-code continuation map. Values are Journey RVAs. The symbolic names
 * state why control returns there rather than leaving bare numeric branches in
 * the repair fragments. */
.set next_grain, blob_start + 0x2c8641 - REGION_RVA
.set cached_grain, blob_start + 0x2c864b - REGION_RVA
.set create_args, blob_start + 0x2c86fd - REGION_RVA
.set account_sound, blob_start + 0x2c87db - REGION_RVA
.set play_found, blob_start + 0x2c0e44 - REGION_RVA
.set play_cleanup, blob_start + 0x2c0ef1 - REGION_RVA
.set creator_resume, blob_start + 0x2ca102 - REGION_RVA
.set creator_fail, blob_start + 0x2ca1b2 - REGION_RVA
.set aligned_malloc_iat, blob_start + 0x5539d8 - REGION_RVA
.set aligned_free_iat, blob_start + 0x5539e0 - REGION_RVA
.set release_iat, blob_start + 0x554050 - REGION_RVA
.set string_destroy, blob_start + 0x7b7c0 - REGION_RVA
.set record_delete, blob_start + 0x499df4 - REGION_RVA
.set insert_qword, blob_start + 0x2d6c10 - REGION_RVA
.set memmove_native, blob_start + 0x49ba82 - REGION_RVA

/* Jumped-to fragment of the stock loader: unchanged RSP and saved-register
 * layout. Key uses the low 32 bits of stock's 64-bit CVTTSS2SI result. */
first_search:
 mov r8, [rdi+0x68]
 lea rsi, [rdi+0x68]
 mov ebx, ecx
 add rbx, [rbp-0x48]
 movsxd r10, DWORD PTR [rbx+0x44]
 add r10, [rbp-0x40]
 mov edx, [rbx+0x44]
 cvttss2si r9, DWORD PTR [rbx+0x4c]
 shl r9, 32
 or rdx, r9
 mov [rsp+0x38], rdx
 mov rax, r8
 mov rcx, [r8+8]
first_loop:
 cmp BYTE PTR [rcx+0x19], 0
 jne first_done
 cmp [rcx+0x20], rdx
 jae first_left
 mov rcx, [rcx+0x10]
 jmp first_loop
first_left:
 mov rax, rcx
 mov rcx, [rcx]
 jmp first_loop
first_done:
 cmp rax, r8
 je first_miss
 cmp [rax+0x20], rdx
 je cached_grain
first_miss:
 jmp create_args

creator_return:
 mov rdi, rax
 mov [rsp+0x60], rax
 test rax, rax
 je next_grain
 cmp QWORD PTR [rax+0x20], 0
 je discard_record
 mov rdx, [rsp+0x38]
 mov rax, [rsi]
 mov rbx, rax
 mov rcx, [rax+8]
second_loop:
 cmp BYTE PTR [rcx+0x19], 0
 jne second_done
 cmp [rcx+0x20], rdx
 jae second_left
 mov rcx, [rcx+0x10]
 jmp second_loop
second_left:
 mov rbx, rcx
 mov rcx, [rcx]
 jmp second_loop
second_done:
 cmp rbx, rax
 je new_identity
 cmp [rbx+0x20], rdx
 je discard_record
new_identity:
 /* Reserve before linking a node. Null failure leaves both containers valid. */
 mov rcx, [rbp+0x530]
 add rcx, 0xb0
 call reserve_vector
 test eax, eax
 je discard_record
 mov ecx, 0x30
 mov edx, 0x10
 call QWORD PTR [rip+aligned_malloc_iat]
 test rax, rax
 je discard_record
 mov rcx, [rsi]
 mov [rax], rcx
 mov [rax+8], rcx
 mov [rax+0x10], rcx
 mov QWORD PTR [rax+0x18], 0
 mov rdx, [rsp+0x38]
 mov [rax+0x20], rdx
 mov QWORD PTR [rax+0x28], 0
 mov [rsp+0x20], rax
 lea r9, [rax+0x20]
 mov r8, rbx
 lea rdx, [rbp+0x38]
 mov rcx, rsi
 call insert_qword
 mov rbx, [rbp+0x38]
 cmp QWORD PTR [rbx+0x28], 0
 jne discard_record
 /* No allocation, API call, or possible reentry between append and map fill. */
 mov rcx, [rbp+0x530]
 mov rax, [rcx+0xb8]
 mov [rax], rdi
 add rax, 8
 mov [rcx+0xb8], rax
 mov [rbx+0x28], rdi
 mov rdi, rcx
 jmp account_sound
discard_record:
 mov rcx, [rdi+0x20]
 test rcx, rcx
 je discard_record_only
 call QWORD PTR [rip+release_iat]
discard_record_only:
 mov rcx, rdi
 call string_destroy
 mov rcx, rdi
 mov edx, 0x58
 call record_delete
 jmp next_grain
loader_fragments_end:

/* The bank reference was retained at stock dispatch entry. On miss, use its
 * matching cleanup path, before temporary Tone parameters are constructed. */
playback_search:
 mov rdx, [r13+0x68]
 mov ecx, [r10+0x44]
 cvttss2si r8, DWORD PTR [r10+0x4c]
 shl r8, 32
 or rcx, r8
 mov rbx, rdx
 mov rax, [rdx+8]
playback_loop:
 cmp BYTE PTR [rax+0x19], 0
 jne playback_done
 cmp [rax+0x20], rcx
 jae playback_left
 mov rax, [rax+0x10]
 jmp playback_loop
playback_left:
 mov rbx, rax
 mov rax, [rax]
 jmp playback_loop
playback_done:
 cmp rbx, rdx
 je play_cleanup
 cmp [rbx+0x20], rcx
 jne play_cleanup
 cmp QWORD PTR [rbx+0x28], 0
 je play_cleanup
 jmp play_found
playback_end:

/* Stock memset used the allocation result before checking it. Release the
 * already-created Sound on null and return through the native failure exit. */
creator_alloc_guard:
 test rax, rax
 je creator_alloc_failed
 xor edx, edx
 mov rcx, rax
 mov rbx, rax
 jmp creator_resume
creator_alloc_failed:
 mov rcx, [rsp+0x150]
 test rcx, rcx
 je creator_fail
 call QWORD PTR [rip+release_iat]
 jmp creator_fail
creator_alloc_end:

/* Standalone Win64 function, with its own unwind information. Matches stock
 * vector buffer alignment/layout, including raw allocation at aligned[-8]
 * when capacity >= 0x1000. This is fallible reserve, not vector append.
 * Its CRT and memmove calls carry no game callback or shared table state. */
.seh_proc reserve_vector
reserve_vector:
 push rbx
 .seh_pushreg rbx
 push rsi
 .seh_pushreg rsi
 push rdi
 .seh_pushreg rdi
 push r12
 .seh_pushreg r12
 sub rsp, 0x28
 .seh_stackalloc 0x28
 .seh_endprologue
 mov rbx, rcx
 mov rax, [rcx+8]
 cmp rax, [rcx+0x10]
 jb reserve_ok
 jne reserve_fail
 sub rax, [rcx]
 jc reserve_fail
 mov rsi, rax
 mov rdi, rax
 shr rdi, 1
 add rdi, rax
 jc reserve_fail
 add rdi, 7
 jc reserve_fail
 and rdi, -8
 test rdi, rdi
 jne reserve_size_ready
 mov edi, 8
reserve_size_ready:
 mov rcx, rdi
 cmp rdi, 0x1000
 jb reserve_alloc
 add rcx, 0x27
 jc reserve_fail
reserve_alloc:
 mov edx, 0x10
 call QWORD PTR [rip+aligned_malloc_iat]
 test rax, rax
 je reserve_fail
 mov r12, rax
 cmp rdi, 0x1000
 jb reserve_copy
 add r12, 0x27
 and r12, -32
 mov [r12-8], rax
reserve_copy:
 test rsi, rsi
 je reserve_free_old
 mov rcx, r12
 mov rdx, [rbx]
 mov r8, rsi
 call memmove_native
reserve_free_old:
 mov rcx, [rbx]
 test rcx, rcx
 je reserve_publish
 cmp rsi, 0x1000
 jb reserve_free
 mov rcx, [rcx-8]
reserve_free:
 call QWORD PTR [rip+aligned_free_iat]
reserve_publish:
 mov [rbx], r12
 lea rax, [r12+rsi]
 mov [rbx+8], rax
 add r12, rdi
 mov [rbx+0x10], r12
reserve_ok:
 mov eax, 1
 jmp reserve_exit
reserve_fail:
 xor eax, eax
reserve_exit:
 add rsp, 0x28
 pop r12
 pop rdi
 pop rsi
 pop rbx
 ret
reserve_vector_end:
.seh_endproc
blob_end:
