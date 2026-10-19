#ifndef RELIEFMAIL_SMTP_H
#define RELIEFMAIL_SMTP_H

#include "mime.h"
#include "transport.h"

namespace rmail {

enum class SubmitOutcome : uint8_t { Accepted, Rejected, Uncertain };
using SendStageCallback = bool (*)(void *context, SendStage stage);

struct SendObserver {
    void *context = nullptr;
    SendStageCallback advance = nullptr;
};

struct SmtpCapabilities {
    bool starttls = false;
    bool auth_plain = false;
    bool auth_login = false;
    bool size = false;
    uint64_t maximum_size = 0;
    bool smtp_utf8 = false;
    bool eight_bit_mime = false;
};

class SmtpSession {
    Stream stream_;
    Account account_;
    void *protocol_state_ = nullptr;
    bool connected_ = false;
    bool authenticated_ = false;

public:
    SmtpSession() = default;
    ~SmtpSession();
    SmtpSession(const SmtpSession &) = delete;
    SmtpSession &operator=(const SmtpSession &) = delete;

    bool connect(const Account &account, const OperationContext &context, Text &error);
    bool authenticate(SecretView password, const OperationContext &context, Text &error);
    void close();

    /* The observer is called with SendStage::SendingData after every RCPT has
     * succeeded and before the DATA command is written. Refusal guarantees no
     * DATA command or message bytes are sent. A lost final reply is Uncertain. */
    SubmitOutcome submit(const Envelope &envelope, const MessageSource &message,
        SendObserver &observer, const OperationContext &context, Text &error);
    bool capabilities(SmtpCapabilities &snapshot) const;
    bool diagnostics(TransportDiagnostics &snapshot) const;
};

}

#endif
