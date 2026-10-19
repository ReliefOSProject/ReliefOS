#include "model.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

using namespace rmail;

static bool text_is(const Text &text, const char *expected) {
    const size_t length = strlen(expected);
    return text.size() == length && memcmp(text.c_str(), expected, length) == 0;
}

static bool empty_attachment_source(void *, size_t, unsigned char *, size_t, size_t *read_size) {
    *read_size = 0;
    return true;
}

static void test_binary_bytes_keep_embedded_nuls_and_length() {
    const unsigned char first[] = {'a', 0, 'b'};
    const unsigned char second[] = {'a', 0, 'c'};
    Bytes a, b, short_value;
    assert(a.set(first, sizeof(first)));
    assert(b.set(second, sizeof(second)));
    assert(short_value.set(first, 1));
    assert(a.size() == 3 && a.data()[0] == 'a' && a.data()[1] == 0 && a.data()[2] == 'b');
    assert(a != b && a != short_value);
    Bytes shared = a;
    assert(shared == a);
    assert(shared.data() == a.data());
}

static void test_message_identity_includes_account_folder_and_uidvalidity() {
    MessageKey first;
    first.folder.account_id = 7;
    assert(first.folder.server_name.set("INBOX"));
    first.uidvalidity = 91;
    first.uid = 42;
    MessageKey same = first;
    MessageKey other_account = first;
    other_account.folder.account_id = 8;
    MessageKey other_folder = first;
    assert(other_folder.folder.server_name.set("Archive"));
    MessageKey other_validity = first;
    other_validity.uidvalidity++;

    assert(first == same);
    assert(first != other_account);
    assert(first != other_folder);
    assert(first != other_validity);
    assert(first.uid == other_account.uid && first.uid == other_folder.uid &&
        first.uid == other_validity.uid);
}

static void test_account_storage_stops_at_sixteen_without_losing_entries() {
    AccountList accounts;
    for (uint64_t id = 1; id <= MaxAccounts; ++id) {
        Account account;
        account.id = id;
        assert(accounts.push(static_cast<Account &&>(account)));
    }
    Account seventeenth;
    seventeenth.id = 17;
    assert(!accounts.push(static_cast<Account &&>(seventeenth)));
    assert(accounts.size() == 16);
    assert(accounts[0].id == 1 && accounts[15].id == 16);
}

static void test_folder_message_page_and_attachment_collections_enforce_limits() {
    FolderList folders;
    assert(folders.reserve(MaxFoldersPerAccount));
    assert(!folders.reserve(MaxFoldersPerAccount + 1));
    assert(folders.resize(MaxFoldersPerAccount));
    Folder folder;
    assert(!folders.push(static_cast<Folder &&>(folder)));

    MessageIndex index;
    assert(!index.reserve(MaxIndexedMessagesPerFolder + 1));
    MessagePage page;
    assert(page.resize(MaxRowsPerPage));
    assert(!page.resize(MaxRowsPerPage + 1));
    assert(page.size() == MaxRowsPerPage);

    AttachmentList attachments;
    assert(attachments.resize(MaxMimeParts));
    Attachment extra;
    assert(!attachments.push(static_cast<Attachment &&>(extra)));
}

static void test_utf8_display_names_and_quoted_local_parts_round_trip() {
    Array<Address> addresses;
    Text error;
    assert(parse_addresses("张三 <\"first.last\"@example.com>, \"Ops, Team\" <alerts+build@example.cn>",
        addresses, error));
    assert(addresses.size() == 2);
    assert(text_is(addresses[0].display_name, "张三"));
    assert(text_is(addresses[0].local_part, "first.last"));
    assert(addresses[0].local_part_quoted);
    assert(text_is(addresses[0].domain, "example.com"));
    assert(text_is(addresses[1].display_name, "Ops, Team"));
    assert(addresses[1].display_name_quoted);

    Text serialized;
    assert(serialize_address(addresses[0], serialized, error));
    assert(text_is(serialized, "张三 <\"first.last\"@example.com>"));
    assert(serialize_address(addresses[1], serialized, error));
    assert(text_is(serialized, "\"Ops, Team\" <alerts+build@example.cn>"));
}

static void test_address_parser_rejects_empty_injection_and_internationalized_addr_specs() {
    Array<Address> addresses;
    Text error;
    assert(!parse_addresses("", addresses, error));
    assert(!error.empty());
    assert(!parse_addresses("user@example.com\r\nBcc: victim@example.net", addresses, error));
    assert(!parse_addresses("张三@example.com", addresses, error));
    assert(text_is(error, "unsupported internationalized address"));
    assert(!parse_addresses("user@例子.中国", addresses, error));

    const char embedded_nul[] = {'u', 's', 'e', 'r', '@', 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        '.', 'c', 'o', 'm', 0, '\r', '\n'};
    assert(!parse_addresses(embedded_nul, sizeof(embedded_nul), addresses, error));
    assert(text_is(error, "header injection is not allowed"));

    Error detailed;
    assert(!parse_addresses("张三@example.com", addresses, detailed));
    assert(detailed.code == ErrorCode::UnsupportedInternationalizedAddress);

    Address internationalized;
    assert(internationalized.local_part.set("用戶"));
    assert(internationalized.domain.set("example.com"));
    Text serialized;
    assert(!serialize_address(internationalized, serialized, error));
    assert(text_is(error, "unsupported internationalized address"));

    assert(!serialize_address(internationalized, serialized, detailed));
    assert(detailed.code == ErrorCode::UnsupportedInternationalizedAddress);
}

static void test_account_validation_checks_ports_and_header_safe_text() {
    Account account;
    account.id = 1;
    assert(account.display_name.set("小明"));
    assert(account.email_address.set("\"first.last\"@example.com"));
    assert(account.username.set("xiaoming"));
    assert(account.incoming_host.set("imap.example.com"));
    assert(account.outgoing_host.set("smtp.example.com"));
    Text error;
    assert(validate_account(account, error));

    account.incoming_port = 0;
    assert(!validate_account(account, error));
    assert(text_is(error, "invalid server port"));
    account.incoming_port = 993;
    account.outgoing_port = 65536;
    assert(!validate_account(account, error));
    assert(text_is(error, "invalid server port"));
    account.outgoing_port = 587;
    const char bad_name[] = {'x', 0, 'y'};
    assert(account.display_name.set(bad_name, sizeof(bad_name)));
    assert(!validate_account(account, error));
    assert(text_is(error, "header injection is not allowed"));

    Error detailed;
    assert(!validate_account(account, detailed));
    assert(detailed.code == ErrorCode::HeaderInjection);
}

static void test_extended_account_preferences_and_delivery_stages_are_validated() {
    Account account;
    account.id = 2;
    assert(account.display_name.set("林小明"));
    assert(account.email_address.set("ming@example.cn"));
    assert(account.username.set("imap-user"));
    assert(account.smtp_username.set("different-smtp-user"));
    assert(account.incoming_host.set("imap.example.cn"));
    assert(account.outgoing_host.set("smtp.example.cn"));
    assert(account.custom_ca_file.set("/home/user/ca.pem"));
    assert(account.inbox_folder.set("INBOX"));
    assert(account.sent_folder.set("已发送"));
    account.enabled = true;
    account.is_default = true;
    account.connect_timeout_ms = 15000;
    account.io_timeout_ms = 30000;
    account.poll_interval_minutes = 15;
    account.cache_days = 30;
    account.signature_id = 8;
    Text error;
    assert(validate_account(account, error));

    account.connect_timeout_ms = 999;
    assert(!validate_account(account, error));
    account.connect_timeout_ms = 15000;
    account.poll_interval_minutes = 1441;
    assert(!validate_account(account, error));
    account.poll_interval_minutes = 15;
    account.cache_days = 3651;
    assert(!validate_account(account, error));

    Folder folder;
    folder.uidvalidity = 17;
    assert(folder.uidvalidity == 17);
    Draft draft;
    draft.send_stage = SendStage::Accepted;
    assert(validate_draft(draft, error));
    draft.send_stage = SendStage::ArchivePending;
    assert(validate_draft(draft, error));
    draft.send_stage = SendStage::ArchiveFailed;
    assert(validate_draft(draft, error));
    draft.send_stage = SendStage::Archived;
    assert(validate_draft(draft, error));

    const unsigned char editor_bytes[] = {'W', 0, 'D', 0xff};
    assert(draft.editor_state.set(editor_bytes, sizeof(editor_bytes)));
    assert(validate_draft(draft, error));
    assert(draft.editor_state.size() == sizeof(editor_bytes));
    assert(memcmp(draft.editor_state.data(), editor_bytes, sizeof(editor_bytes)) == 0);
    assert(draft.editor_state.set(nullptr, 0));
    assert(validate_draft(draft, error));
    unsigned char *oversized_editor = static_cast<unsigned char *>(malloc(MaxEditorStateBytes + 1));
    assert(oversized_editor);
    memset(oversized_editor, 0xa5, MaxEditorStateBytes + 1);
    assert(draft.editor_state.set(oversized_editor, MaxEditorStateBytes + 1));
    assert(!validate_draft(draft, error));
    free(oversized_editor);
}

static void test_mailto_percent_decodes_subject_and_body_into_unsent_draft() {
    Draft draft;
    draft.send_stage = SendStage::Failed;
    Text error;
    assert(parse_mailto("mailto:%22first.last%22@example.com?subject=Hello%20world&body=Line%201%0ASecond%20line",
        draft, error));
    assert(draft.to.size() == 1 && draft.cc.empty() && draft.bcc.empty());
    assert(text_is(draft.to[0].local_part, "first.last"));
    assert(draft.to[0].local_part_quoted);
    assert(text_is(draft.subject, "Hello world"));
    assert(text_is(draft.body, "Line 1\nSecond line"));
    assert(draft.send_stage == SendStage::Draft);

    Draft unchanged;
    assert(unchanged.subject.set("keep"));
    assert(!parse_mailto("mailto:user@example.com?subject=bad%0D%0ABcc%3Ax", unchanged, error));
    assert(text_is(unchanged.subject, "keep"));
    assert(!parse_mailto("mailto:user@example.com?body=%00", unchanged, error));
    const char mailto_nul[] = {'m','a','i','l','t','o',':','u','@','e','.','c','o','m',0,'\n'};
    assert(!parse_mailto(mailto_nul, sizeof(mailto_nul), unchanged, error));
}

static void test_draft_validation_enforces_text_and_total_attachment_limits() {
    Draft draft;
    Text error;
    assert(draft.subject.set("Monthly report"));
    assert(draft.body.set("你好\nAttached."));
    assert(validate_draft(draft, error));

    void *payload = malloc(DefaultMaxAttachmentBytes);
    assert(payload);
    memset(payload, 0x5a, DefaultMaxAttachmentBytes);
    Attachment attachment;
    assert(attachment.filename.set("report.bin"));
    assert(attachment.content_type.set("application/octet-stream"));
    assert(attachment.data.set(payload, DefaultMaxAttachmentBytes));
    free(payload);
    assert(draft.attachments.push(static_cast<Attachment &&>(attachment)));
    assert(validate_draft(draft, error));

    Attachment extra;
    assert(extra.filename.set("extra.bin"));
    assert(extra.content_type.set("application/octet-stream"));
    assert(extra.data.set("x", 1));
    assert(draft.attachments.push(static_cast<Attachment &&>(extra)));
    assert(!validate_draft(draft, error));
    assert(text_is(error, "attachment data exceeds the default limit"));
}

static void test_draft_body_classifies_invalid_utf8_and_forbidden_controls() {
    Draft draft;
    Error error;

    const char malformed_utf8[] = {static_cast<char>(0xe4), static_cast<char>(0xb8)};
    assert(draft.body.set(malformed_utf8, sizeof(malformed_utf8)));
    assert(!validate_draft(draft, error));
    assert(error.code == ErrorCode::InvalidUtf8);

    const char embedded_nul[] = {'a', 0, 'b'};
    assert(draft.body.set(embedded_nul, sizeof(embedded_nul)));
    assert(!validate_draft(draft, error));
    assert(error.code == ErrorCode::HeaderInjection);

    const char forbidden_control[] = {'a', 1, 'b'};
    assert(draft.body.set(forbidden_control, sizeof(forbidden_control)));
    assert(!validate_draft(draft, error));
    assert(error.code == ErrorCode::HeaderInjection);

    assert(draft.body.set("first line\nsecond\tline"));
    assert(validate_draft(draft, error));
}

static void test_draft_rich_fields_and_streaming_attachment_limits() {
    Draft draft;
    assert(draft.body.set("plain"));
    assert(draft.html_body.set("<p>rich</p>"));
    draft.rich_format = true;
    assert(draft.message_id.set("<new@example.com>"));
    assert(draft.in_reply_to.set("<parent@example.com>"));
    assert(draft.references.set("<root@example.com> <parent@example.com>"));
    Text error;
    assert(validate_draft(draft, error));

    Attachment streamed;
    assert(streamed.filename.set("large.bin"));
    assert(streamed.content_type.set("application/octet-stream"));
    streamed.source.read = empty_attachment_source;
    streamed.source.size = DefaultMaxAttachmentBytes;
    assert(draft.attachments.push(static_cast<Attachment &&>(streamed)));
    assert(validate_draft(draft, error));
    draft.attachments[0].source.size++;
    assert(!validate_draft(draft, error));
    assert(text_is(error, "attachment data exceeds the default limit"));

    draft.attachments[0].source.size = 0;
    const char malformed[] = {static_cast<char>(0xe4), static_cast<char>(0xb8)};
    assert(draft.html_body.set(malformed, sizeof(malformed)));
    assert(!validate_draft(draft, error));
    assert(text_is(error, "draft HTML body is not valid UTF-8"));
}

static void test_raw_recipient_inputs_preserve_incomplete_source_separately() {
    Draft draft;
    Text parse_error;
    assert(parse_addresses("stale@example.net", draft.to, parse_error));
    Text error;
    const char invalid_to[] = "unfinished address";
    assert(set_raw_recipient_inputs(draft, invalid_to, sizeof(invalid_to) - 1,
        "", 0, "", 0, error));
    assert(draft.raw_recipient_input);
    assert(text_is(draft.to_input, "unfinished address"));
    assert(draft.to.size() == 1 && text_is(draft.to[0].local_part, "stale"));

    Draft authoritative;
    Address stale_invalid;
    assert(stale_invalid.display_name.set("Bad\r\nBcc: victim@example.net"));
    assert(stale_invalid.local_part.set("old"));
    assert(stale_invalid.domain.set("example.net"));
    assert(authoritative.to.push(static_cast<Address &&>(stale_invalid)));
    assert(set_raw_recipient_inputs(authoritative, "new@example.net", strlen("new@example.net"),
        "", 0, "", 0, error));
    assert(validate_draft(authoritative, error));
}

static void test_mailto_subject_limit_is_reported_as_a_size_error() {
    const char prefix[] = "mailto:?subject=";
    const size_t prefix_length = sizeof(prefix) - 1;
    const size_t uri_length = prefix_length + MaxEditableTextBytes + 1;
    char *uri = static_cast<char *>(malloc(uri_length));
    assert(uri);
    memcpy(uri, prefix, prefix_length);
    memset(uri + prefix_length, 'x', MaxEditableTextBytes + 1);
    Draft draft;
    Error error;
    assert(!parse_mailto(uri, uri_length, draft, error));
    assert(error.code == ErrorCode::LimitExceeded);
    free(uri);
}

static void test_contact_signature_template_rule_and_attachment_types_are_concrete() {
    Contact contact;
    contact.id = 11;
    assert(contact.display_name.set("小林"));
    Address address;
    assert(address.local_part.set("lin"));
    assert(address.domain.set("example.org"));
    assert(contact.addresses.push(static_cast<Address &&>(address)));

    Signature signature;
    signature.id = 12;
    assert(signature.name.set("Work"));
    assert(signature.body.set("Regards"));

    Template message_template;
    message_template.id = 13;
    assert(message_template.name.set("Weekly update"));
    assert(message_template.subject.set("Status"));
    assert(message_template.body.set("Progress:"));

    Rule rule;
    rule.id = 14;
    rule.field = RuleField::Subject;
    rule.operation = RuleOperator::Contains;
    assert(rule.value.set("invoice"));
    rule.action = RuleAction::Move;
    rule.destination.account_id = 1;
    assert(rule.destination.server_name.set("Archive"));

    Attachment attachment;
    assert(attachment.filename.set("document.pdf"));
    assert(attachment.data.set("pdf", 3));
    assert(attachment.data.size() == 3);
    assert(contact.id == 11 && signature.enabled && message_template.id == 13 &&
        rule.action == RuleAction::Move);
}

int main() {
    test_binary_bytes_keep_embedded_nuls_and_length();
    test_message_identity_includes_account_folder_and_uidvalidity();
    test_account_storage_stops_at_sixteen_without_losing_entries();
    test_folder_message_page_and_attachment_collections_enforce_limits();
    test_utf8_display_names_and_quoted_local_parts_round_trip();
    test_address_parser_rejects_empty_injection_and_internationalized_addr_specs();
    test_account_validation_checks_ports_and_header_safe_text();
    test_extended_account_preferences_and_delivery_stages_are_validated();
    test_mailto_percent_decodes_subject_and_body_into_unsent_draft();
    test_draft_validation_enforces_text_and_total_attachment_limits();
    test_draft_body_classifies_invalid_utf8_and_forbidden_controls();
    test_draft_rich_fields_and_streaming_attachment_limits();
    test_raw_recipient_inputs_preserve_incomplete_source_separately();
    test_mailto_subject_limit_is_reported_as_a_size_error();
    test_contact_signature_template_rule_and_attachment_types_are_concrete();
    return 0;
}
