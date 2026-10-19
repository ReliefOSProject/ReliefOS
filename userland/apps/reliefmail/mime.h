#ifndef RELIEFMAIL_MIME_H
#define RELIEFMAIL_MIME_H

#include "model.h"

namespace rmail {

struct ParsedMessage {
    Text from;
    Text reply_to;
    Text to;
    Text cc;
    Text bcc;
    Text subject;
    Text date;
    Text message_id;
    Text in_reply_to;
    Text references;
    Text plain_text;
    Text html_text;
    Text warning;
    AttachmentList attachments;
    Bytes original_message;
};

struct Envelope {
    Address sender;
    Array<Address> recipients;
};

using MimeWriteCallback = bool (*)(void *context, const unsigned char *bytes, size_t size);

struct MimeSink {
    MimeWriteCallback write = nullptr;
    void *context = nullptr;
    size_t bytes_written = 0;
    Text error;
};

bool parse_message(const char *bytes, size_t length, ParsedMessage &output, Text &error);
bool decode_modified_utf7(const char *bytes, size_t length, Text &output, Text &error);

bool encode_message(const Draft &draft, const Account &account, MimeSink &sink,
    Envelope &envelope, Text &error);
bool encode_message(const Draft &draft, const Account &account, Bytes &output,
    Envelope &envelope, Text &error);

}
#endif
