# ReliefMail Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for native execution, or superpowers:subagent-driven-development when the user selects delegation. Steps use checkbox syntax for tracking.

**Goal:** Build a working multi-account Chinese IMAP/SMTP desktop mail client matching ReliefExcel/ReliefWord, and deliver screenshots of the application running in ReliefOS.

**Architecture:** Independent C++17 mail core, MIME/address parsing, SQLite index and raw-message cache, existing mbedTLS transport and credential encryption, bounded worker-process IPC, and FLTK main/compose/tool windows. Reuse ReliefWord model/layout capabilities through mail-specific adapters without changing Word semantics. Persist delivery stages so a lost SMTP reply never becomes an automatic duplicate submission.

**Tech Stack:** C++17 without exceptions/RTTI or C++ standard-library headers; FLTK 1.3; gettext; existing mbedTLS 3.6.7 runtime configuration; private POSIX build of the existing SQLite amalgamation (legacy system VFS unchanged); POSIX sockets/poll/pipes/processes; host C++/Python protocol tests; QEMU private test image.

**Spec:** docs/superpowers/specs/2026-10-18-reliefmail-design.md (approved in this conversation).

## Global Constraints

- Work in /home/xiaobai/Projects/Projects/ReliefOS_Leon_WorkSpace on rewrite/desktop-apps; do not commit, push or create a branch without new explicit authorization.
- Preserve unrelated files and changes, kernel/reliefnt, audio manifest semantics, ABI golden, theme/background and existing Office behavior. Never enumerate all untracked files under result.
- Temporary outputs use the current repository out/tmp; build/reliefmail-dev and out contain artifacts only. Do not commit out, build, result, .zcode or tools/dependencies.py.
- Use the existing 34 px Office tab/menu row, approximately 100 px functional ribbon, native FLTK controls, SimSun/gettext and approximately 28 px status bar. No duplicate application heading inside the window.
- Password/application-password authentication; no OAuth2 claim. TLS is required for authentication, including mandatory STARTTLS when selected. Existing TLS 1.2 support is retained.
- Optional credentials vault: PBKDF2-HMAC-SHA256 with 600,000 iterations, random salt, AES-256-GCM, fresh nonce, version and authenticated metadata; never put credentials in arguments/environment/logs.
- Limits: 16 accounts, 4,096 folders per account listing, 100,000 indexed messages per folder, 200 rows per page, 64 MiB raw message, 1 MiB editable UTF-8 text, MIME depth 20, 512 MIME parts, 256 KiB headers, default 25 MiB total attachments. Word's existing 500,000-scalar bound remains an additional editor bound when its model is used.
- Network concurrency at most two; at most one server-mutating task per account. No network wait, large parse, lock wait or cache maintenance in GUI callbacks. No database transaction spans a network operation.
- Use a private standard Unix/POSIX VFS build of the same SQLite amalgamation, DELETE journal and synchronous FULL; preserve the existing system SQLite module.
- Track account/folder/UIDVALIDITY/UID identity and UI request generation. Never use unrestricted EXPUNGE as a MOVE fallback.
- A DATA-stage lost response produces an uncertain state requiring user review; sent-folder APPEND failure retries archiving only.
- Verify host tests, target build and QEMU runtime separately. Provide at least three real screenshots. VMware 验证未完成; real hardware and QQ/163 live-account validation remain unverified until actually tested.

## Review Focus

- Two accounts or folders share a UID: selecting or modifying one must not affect the other; Task 1 and Task 5 pin identity and late-result rejection.
- A server streams a huge literal or never terminates a MIME part: limits must reject the input without truncating a saved/sent message or freezing the GUI; Task 2 and Task 4 pin fragmented and bounded parsing.
- A connection fails just after the server accepts SMTP DATA: restart must preserve uncertainty and must not resend; Task 5 pins persistent stage transitions and Task 11 verifies server delivery counts.
- A tampered vault, wrong master password or symlinked data path is opened: original data must survive and no file outside the profile may be overwritten; Task 3 pins authentication failure and path ownership.
- A stale sync result arrives after account removal or folder UIDVALIDITY reset: neither UI nor rules may apply it to a replacement identity; Task 5 and Task 8 pin generation checking and once-only rule execution.

## File Map and Shared Interfaces

New application sources live in userland/apps/reliefmail. Use namespace rmail and explicit-error bool/Result interfaces, with Text/Array/Bytes storage compatible with existing Office allocation behavior. Define shared interfaces once in headers, then implement against those declarations.

- storage.h, model.h/model.cpp: limits, IDs, Account, FolderKey, MessageKey, MessageSummary, Attachment, Draft, Contact, Signature, Template, Rule, SendStage, Result/Error; address/mailto validation.
- mime.h/mime.cpp, html.h/html.cpp: ParsedMessage, MIME codec, character-set conversion, body alternatives, safe HTML, RFC headers and attachment serialization.
- sqlite_core.c: private standard Unix/POSIX SQLite build, rollback journal and FULL sync; no link to the legacy no-op VFS.
- store.h/store.cpp, vault.h/vault.cpp: ProfilePaths, Store, file/cache transactions, persisted delivery state and Vault.
- transport.h/transport.cpp, imap.h/imap.cpp, smtp.h/smtp.cpp: Stream, deadline/cancellation, TLS, ImapSession and SmtpSession.
- worker.h/worker.cpp, tasks.h/tasks.cpp: versioned IPC request/result frames, TaskRunner and mail-operation coordinator.
- window.h/window.cpp, reader.cpp, compose.h/compose.cpp, editor.h/editor.cpp, dialogs.cpp, tools.cpp: native UI and mail-specific rich editor.
- main.cpp and reliefmail.desktop: GUI/worker entry and desktop integration.
- tests/host/test_reliefmail_{model,mime,store,vault,protocol,worker}.cpp, tests/build/test-reliefmail.sh, tools/tests/reliefmail_protocol_server.py, tools/tests/reliefmail_guest_verify.cpp, tools/test_reliefmail_qemu.py: behavior and runtime tests.
- Existing integration files: mk/userland.mk, mk/tests.mk, configs/components.toml, Kconfig.components, configs/apk-ownership.json, configs/app-icons.list, configs/nls files, relevant desktop/menu/launcher and console-profile files, app-icon assets. Inspect exact current integration rather than inventing a parallel registration mechanism.

### Task 1: Mail identities, bounded storage and address handling

**Interfaces:** model.h produces Account, FolderKey(account_id, server_name), MessageKey(folder, uidvalidity, uid), Draft and SendStage; bool parse_addresses(const char *, Array<Address> &, Text &error); bool parse_mailto(const char *, Draft &, Text &error); bool validate_account(const Account &, Text &error). Later modules consume these exact declarations.

- [x] Write model tests for two equal UIDs under distinct account/folder/UIDVALIDITY keys; 16-account limit; Chinese display names and quoted local parts; empty recipient; CR/LF/NUL injection; invalid ports; percent-encoded mailto subject/body; mailto must produce a draft and never a send action.
- [x] Run the model suite and confirm missing interfaces fail before implementation. Add the test harness to tests/build/test-reliefmail.sh using out/tmp, ASan/UBSan and the existing no-exceptions flags.
- [x] Implement storage/model with checked growth and UTF-8 boundaries; define shared result and limit values. Address serialization must preserve display names and reject unsupported internationalized address forms explicitly, rather than rewriting addresses.
- [x] Run the model suite to green; inspect allocation/overflow and move ownership at module boundaries. Record completion in this plan; leave changes uncommitted.

Execution record: model implementation and controller ASan/UBSan suite passed; independent review fix round 1 addressed all findings; no new Critical/Important breakage. No commits created.

### Task 2: MIME, safe HTML and draft serialization

**Interfaces:** bool parse_message(const char *, size_t, ParsedMessage &, Text &error); bool encode_message(const Draft &, const Account &, Bytes &, Envelope &, Text &error); bool sanitize_html(const char *, size_t, Text &, Text &warning, Text &error). ParsedMessage contains decoded headers, plain/HTML alternatives, attachment metadata/content references, and original-message access.

- [x] Add MIME tests for Chinese encoded subject and names, folded fields, Base64/QP binary round-trip, RFC 2231 filename continuations, modified UTF-7 folders, mixed/alternative/related and nested messages, missing boundaries and unknown charsets. Exercise the exact depth/part/header/raw limits.
- [x] Add HTML tests for script/style/form/event/remote/file-resource exclusion, visible safe links, CID lookup, and unsupported-content warning. Add outgoing tests asserting a text alternative, attachment hashes, absent Bcc header, maintained reply IDs, and no header injection.
- [x] Run failing tests, then implement MIME and HTML modules using bounded cursors and existing encoding facilities. Retain raw bytes independently of rendered text. Write attachments through streaming interfaces rather than duplicating large data in widget strings.
- [x] Run sanitizer suites and compare generated EML against Python standard-library email parsing; use independent expected bytes and hashes for binary attachment assertions.

Execution record: ASan/UBSan model, MIME/HTML and independent Python EML/hash checks passed. Independent review and fix-round1 scoped review approved; malformed supported-charset replacements now warn. Controller target compile-only model/MIME/HTML check passed; linked app/QEMU remain later tasks. Initial MIME/HTML implementation-round RED artifact could not be recovered and is not claimed verified; the raw-recipient and fix-round RED/GREEN evidence is preserved. No commits. Streaming failures may leave partial sink output, so later sending must finish an atomic spool before SMTP DATA.

### Task 3: Profile store, cache, drafts and encrypted vault

**Interfaces:** bool profile_paths(ProfilePaths &, Text &error); Store::open(const ProfilePaths &, Text &error); account/folder/message/draft/contact/rule CRUD methods; Store::set_send_stage(Id, SendStage, Text &error); Store::recover_interrupted_sends(Text &error); Vault::unlock/save/change_password, consuming Bytes and no GUI pointers.

- [x] Add store tests for 0700/0600 modes, HOME/XDG fallback, existing unsafe symlinks, ownership, instance locking, parameter-bound Chinese/search data, UIDVALIDITY replacement, crash-stage recovery, short transactions, migration backup and newer-schema refusal.
- [x] Add vault tests for independent AES-GCM vectors, PBKDF2 vector, randomized repeated encryption, wrong password, tampered metadata/tag/ciphertext, truncated/oversized file, out-of-range iteration count, failed atomic replacement and successful master-password change. Assert the old file survives each failure and no recognizable password appears in stored bytes.
- [x] Run failing suites. Implement SQLite store and atomic cache writes with openat/O_NOFOLLOW or equivalent checked directory descriptors. Build host crypto against the bundled configuration or a matching version; do not assume host mbedTLS 4.x has the target 3.6 ABI.
- [x] Implement versioned authenticated credential storage, memory zeroization and bounded KDF parameters. Add backup/restore primitives with complete validation before replacement; cache clearing excludes unsent drafts and delivery records.
- [x] Run store/vault tests, including interruption fault injection at write/rename/commit boundaries, and record actual validation output.

Execution record: implementation and host store/vault harness passed; independent review plus fix rounds1-2 are clean. Restores authenticate an included vault and use a durable crash-recovery journal; explicit commit/cleanup/recovery statuses and RAII attachment sources are covered. Host SIGKILL recovery tests passed; strict target source/private SQLite builds passed with generated-only warning handling scoped to its wrapper. Controller uid1000 QEMU backend probe guest-04 passed normal Store/Vault/restore/ownership paths, source image unchanged. Crash matrix remains host evidence; integrated Mail GUI/network acceptance is later. Actual initial RED and later mutation-sensitivity evidence are distinguished in task-3-report.md. No commits.

### Task 4: TLS transport and real IMAP/SMTP sessions

**Interfaces:** Stream::connect(host, port, tls_mode, ca_path, deadlines, error), read/write/start_tls/close; ImapSession::login/list/select/fetch/search/store/copy/move/append/folder_operation; SmtpSession::authenticate and submit(const Envelope &, MessageSource &, SendObserver &, Text &error). Session methods consume model identities and explicit cancellation/deadline context.

- [x] Build an independent scripted IMAP/SMTP server fixture with TLS and STARTTLS, scripted capability sets, split packets, literals, multiline replies, slow responses and deliberate disconnects. Test mandatory certificate verification, wrong-host/expired/untrusted CA, authentication failure and refusal to downgrade.
- [x] Add IMAP tests for quoted/literal folders, UIDVALIDITY, STATUS/FETCH unrequested responses, LOGIN/PLAIN, optional ID, bounded UID batches, non-MOVE/UIDPLUS combinations and safe refusal where targeted deletion is unavailable.
- [x] Add SMTP tests for EHLO/AUTH PLAIN/LOGIN, SIZE, CRLF and dot-stuffing, all-recipient validation before DATA, one refused RCPT, partial writes, failed final reply and APPEND-only retry boundaries. Confirm the tests fail before implementation.
- [x] Implement direct existing mbedTLS APIs with POSIX socket/poll callbacks, CA/hostname/time validation and TLS 1.2, without changing exported runtime ABI. Implement protocol tokenizers/state machines that consume fragmented input and enforce limits before allocation.
- [x] Run protocol tests; verify independent server transcripts contain expected commands and no unencrypted authentication. Cross-check QQ/163 preset parameters against official provider material and document sources.

Execution record: implementation, focused RED/GREEN and final host harness passed; strict SDK compile/shared runtime link passed. Controller uid1000 guest04 normal protocol interoperability passed both TLS modes against isolated Dovecot/Postfix with independent delivery/MIME/attachment checks, source image unchanged. Independent review and fix-round1 scoped re-review are clean. Exact final-source uid1000 guest06 also passed, with all19 frozen inputs matching after run. DNS resolver watchdog remains Task5; no GUI/provider/VMware acceptance claim. No commits.

### Task 5: Worker IPC, synchronization and durable operations

**Interfaces:** Request/Response frames carry protocol version, bounded size, request ID, account ID and UI generation; TaskRunner::start/poll/cancel; task handlers consume Store plus sessions. Sending consumes only persisted draft/queued IDs and reports durable SendStage changes.

- [x] Add worker tests for split/oversized/invalid frames, EOF, child failures, no argv/env credentials, descriptor cleanup, cancellation escalation and duplicate result IDs. Assert global network concurrency <=2 and account mutation concurrency <=1.
- [x] Add task tests for stale folder/account generation, account deletion during sync, UIDVALIDITY reset, duplicate sync/rule identity, offline queued flag changes, cancellation during fetch, and folder removal with pending work.
- [x] Add a persisted send-stage table test: interrupted pre-submit -> retryable; interrupted DATA/post-submit -> uncertain; SMTP accepted + failed sent APPEND -> archive retry only. Independent server delivery count must stay at one through restart/retry.
- [x] Run failing tests, then implement nonblocking parent IPC, fixed executable worker mode, anonymous credential pipes and bounded queued tasks. Persist server mutations and send stages before irreversible protocol steps; propagate categorized failures to UI without credential content.
- [x] Run worker/protocol/store suites together and demonstrate that cancelled/stale results cannot overwrite a current cached view.

Execution: Task5 complete after two scoped fix/review rounds; all7 findings addressed, final re-review clean. Controller verified final host artifacts, strict normal compile, all27source hashes and nativeguest08 ordinary-user backend PASS. Pre-fix2 nativeb03 independently verifies full-message/attachment bytes; final GUI remains Tasks6–11. Task5 establishes durable queue/identity guards; actual rule-application receipts and offline-queue controls plus their execution tests belong to Task8, as assigned in that task.

### Task 6: Main reader and account setup in Office style

**Interfaces:** MailWindow dispatches rmail::Command to TaskRunner and receives generation-checked events; AccountDialog produces a validated Account plus session credential; ReaderView consumes ParsedMessage and safe resource references only.

- [x] Implement main.cpp and a buildable FLTK window with file menu, Home/Send-Receive/Folders/Tools/View ribbon, resizable three-pane layout, visible-row mail list, status/task bar and existing font/theme initialization. Read the Word/Excel source for exact control style.
- [x] Connect account create/edit/disable/delete/default, separate IMAP/SMTP configuration, provider presets, TLS/CA controls, login tests, special-folder mappings, master-password setup/unlock and session-only credentials to implemented backend actions.
- [x] Wire unified inbox, folder tree, pagination, delayed search, server/local scope, sorting, filters, multiselect, preview/plain/original display, attachment save and user-approved external links. Every toolbar item must either work or be explicitly unavailable with a reason tied to capabilities/state.
- [x] Exercise host/target UI with real cached messages and background slow-server tasks. Check Chinese text fit, minimum size, keyboard focus, resizes, account switching, late responses and responsive cancellation. Use runtime inspection/screenshots for layout; do not create tests that merely repeat widget coordinates.

**Acceptance:** Task6 host/normal-target/native evidence and initial plus scoped fix reviews are recorded in `.superpowers/sdd/2026-10-18-reliefmail/task-6-controller-verification.md`. UI27 closes the default-account vault restore regression. Task7/8 hooks remain explicitly owned by the next tasks; this is not complete-application acceptance. VMware verification remains incomplete.

### UI correction requested by the human — 2026-10-19

The human rejected the current toolbar/layout/UI and requested a complete
redesign to the ReliefWord/ReliefExcel standard. Earlier Task6 functional
verification is retained; its previous visual acceptance is reopened. The
replacement scope is `.superpowers/sdd/2026-10-18-reliefmail/ui-redesign-brief.md`,
grounded in real Word/Excel screenshots captured in the same ReliefOS desktop.
Preserve the implemented mail backend and existing Office sources.

- [x] Replace the main ribbon, account/folder navigation, message rows and reader
  layout with the coherent Office presentation described in the brief.
- [x] Replace compose ribbon/header/attachment/body layout and unify account
  dialogs; use the current Chinese catalog and real enabled/toggle states.
- [x] Pass focused regressions and a strict normal target build, then inspect
  real normal/minimum-size QEMU windows, Chinese clipboard and active controls.
- [x] Complete a fresh scoped GPT-6-Luna/Max review and parent visual acceptance
  before resuming Task7 feature completion and final screenshots.

### Task 7: Rich/plain compose, attachments and delivery flow

**Interfaces:** ComposeWindow owns Draft, a mail-specific rich editor adapter and draft revision; editor exports both safe HTML and plain text, maintains link ranges under edits/undo, and loads a saved draft without format loss.

- [x] Add rich-body round-trip tests for Chinese text, Word-compatible styles/paragraphs, link insert/edit/delete under undo/redo, plain-text conversion, replies/forward quoting and repeated draft autosave. Assert 1 MiB UTF-8 and inherited 500,000-scalar limits are explicit failures.
- [x] Implement the compose window's matching ribbon, account/from/to/cc/bcc/subject fields, font/style/paragraph/list/link controls, attachment list and signature/template insertion. Preserve Chinese IME, clipboard and shortcuts.
- [x] Wire local auto-save/recovery, reopen draft, confirmed format-loss switching, attach/remove/save-as, send/queue/retry and pending-send close handling. Address completion uses contacts; sent-copy retry never reaches SMTP again.
- [x] Test a composed rich message with binary attachment through the real protocol fixture and independent MIME parser; inspect received content and attachment hashes, not just the GUI success label.


**Task7 acceptance:** Task7/fixround1 and UI/fix1–4 independent reviews approved; strict ordinary build, real draft persistence/formatting/attachment, empty-recipient rejection, and actual SMTP plus independent Python MIME/1MiB attachment/unique delivery verification are recorded in `.superpowers/sdd/2026-10-18-reliefmail/task-7-controller-verification.md`. Additional Reply/ReplyAll/Forward/navigation interoperability and final full application screenshots remain Tasks10–11. Resize redraw delay has no diagnosed cause and is not claimed responsiveness-passed. No commits.

### Task 8: Mail actions, folders, contacts, rules and tools

**Interfaces:** Rule matching consumes MessageKey and decoded summary, records applied rule version per message identity, and produces ordinary task requests. Tools use Store/TaskRunner APIs already defined; they do not bypass send confirmation or credential redaction.

- [ ] Add rule tests for order/disable, once-only actions, moved-message reappearance, account isolation and UIDVALIDITY changes. Add backup/restore and EML import/export tests with malformed paths and duplicate message identity.
- [ ] Implement mark/star/move/copy/archive/junk/trash/permanent-delete, subscribe/create/rename/delete folders, confirmation of destructive actions and offline queue review. Preserve safe capability fallback from Task 4.
- [ ] Implement contact CRUD/from-sender/completion, per-account signatures/templates and their editors; implement rule CRUD/order/manual-run/logs with no auto-forward/send action.
- [ ] Implement EML open/import/export, source/header viewer, diagnostics/capabilities/certificate details, operation history, offline mode, cache usage/cleanup, profile/vault backup/validated restore and settings. Use asynchronous tasks for expensive operations.
- [ ] Run relevant model/store/rule/protocol suites, then exercise each tool on a disposable profile and inspect resulting state.

### Task 9: Default image, desktop associations, localization and icon

**Interfaces:** reliefmail is a normal manifest-owned FLTK application; .eml and mailto launch the GUI with one validated input that creates a reader/draft, never an automatic send.

- [ ] Register the app in component selection/default Xorg profile, FLTK build rules and SQLite dependencies, Kconfig, APK ownership, app-icon list and the existing desktop/menu pipeline. Inspect launcher behavior before adding mailto support and preserve other handlers.
- [ ] Add reliefmail.desktop with Office/Network/Email categories and message/rfc822, x-scheme-handler/mailto support. Add an original icon following the existing app-icon system, including BMP and required hicolor sizes; use code-native/vector drawing where appropriate.
- [ ] Add Chinese UI messages through the existing translation pipeline. Ensure console-only packaging excludes the GUI executable and icon.
- [ ] Run component/metadata/ownership, desktop-file, icon and translation checks and a target app-reliefmail build. Verify the actual ELF, dependency resolution and staged desktop/icon contents.

### Task 10: Mature-server interoperability and full target image

- [ ] Run the client against a mature independent IMAP/SMTP test service (prefer an isolated Dovecot-based mail service with a submission service), using test-only accounts and a private test CA. Keep server data, logs, keys and volumes under ignored task artifacts; remove running test services when done.
- [ ] Verify two accounts, Chinese folders/headers, plaintext/HTML alternatives, binary attachments, reply/forward, state changes, folder CRUD, drafts and sent APPEND. Verify server-side delivery counts and resulting mailbox contents through an independent client.
- [ ] Verify failed certificate/authentication, STARTTLS requirement, disconnect/timeout/cancellation, uncertain delivery and no duplicate resend against fixture fault scenarios; distinguish these fixture checks from mature-server interoperability.
- [ ] Build target ELF, translations and image in the current repository. Reuse only read-only downloads from the existing cache if useful; keep build outputs isolated from the sibling repository's ongoing work. Record successful commands, program SHA256 and actual image contents. Do not pipe make through consumers that can terminate it via SIGPIPE.

### Task 11: QEMU runtime, screenshots, review and delivery

- [ ] Extend the existing private-image QEMU harness for ReliefMail, inject the final kernel/app/integration payload, record source and injected hashes, and launch as an ordinary user with dedicated test-server accounts. Original baseline image must remain unchanged.
- [ ] Exercise multi-account Chinese reading, slow background synchronization responsiveness, rich compose/attachment, real send and independent received-message verification, drafts/restart, flags/move, account tests, master-password unlock/error and cancellation recovery.
- [ ] Capture at least main-window, rich-compose and account/tool screenshots from the running ReliefOS desktop; save absolute paths under ignored artifacts, inspect screenshots for clipping/empty/test-only layout, and retain only representative valid frames.
- [ ] Run relevant final host suites and required integration checks once after the final changes; investigate any failures before broadening checks. Conduct a fresh whole-change review using the user-authorized GPT-6-Luna/Max model only if delegation is authorized/applicable; otherwise review inline and report the review limitation honestly.
- [ ] Update checked steps and actual evidence in the plan or verification record. Deliver concise source/build/test/runtime results and real screenshots; explicitly state VMware 验证未完成, unverified real hardware/service-provider accounts and any material capability gaps. Leave changes uncommitted and unpushed.

## Self-Review and Execution Handoff

Coverage: Tasks 1–2 cover model, addresses, MIME and rendering; Task 3 covers store/vault; Tasks 4–5 cover protocols and recovery; Tasks 6–8 cover all approved UI/actions/tools; Task 9 covers system integration; Tasks 10–11 cover independent interoperability, target build, runtime and screenshots. Each Review Focus condition has an owning failing test above. Shared interfaces are defined before consumers and implementation must keep their headers authoritative.

Recommended method: native execution in this conversation. The protocol, store, worker and UI tasks share several evolving interfaces; keeping implementation in one context reduces coordination overhead. Any delegated reviewer must respect the user's GPT-6-Luna/Max restriction and actual available-model allowlist. User approval of this written plan and selection/confirmation of execution method precede implementation.
