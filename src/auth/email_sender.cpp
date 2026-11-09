#include "auth/email_sender.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/log.h"

namespace auth {
namespace {

bool has_header_injection(const std::string& value) {
  return value.find('\r') != std::string::npos || value.find('\n') != std::string::npos;
}

class LogEmailSender final : public EmailSender {
 public:
  bool send(const EmailMessage& message, std::string& error) override {
    if (has_header_injection(message.to) || has_header_injection(message.subject)) {
      error = "email header contains CR/LF";
      return false;
    }
    platform::log_warn("email(dev) to=" + message.to + " subject=" + message.subject +
                       " body=" + message.body);
    return true;
  }
};

struct UploadContext {
  std::string payload;
  std::size_t offset = 0;
};

std::size_t read_callback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
  auto* context = static_cast<UploadContext*>(userdata);
  const std::size_t capacity = size * nitems;
  const std::size_t remaining = context->payload.size() - context->offset;
  const std::size_t count = std::min(capacity, remaining);
  if (count > 0) {
    std::memcpy(buffer, context->payload.data() + context->offset, count);
    context->offset += count;
  }
  return count;
}

class SmtpEmailSender final : public EmailSender {
 public:
  SmtpEmailSender(std::string url, std::string from, bool starttls)
      : url_(std::move(url)), from_(std::move(from)), starttls_(starttls) {}

  bool send(const EmailMessage& message, std::string& error) override {
    if (has_header_injection(message.to) || has_header_injection(from_) ||
        has_header_injection(message.subject)) {
      error = "email header contains CR/LF";
      return false;
    }

    UploadContext context;
    context.payload = "From: " + from_ + "\r\n" +
                      "To: " + message.to + "\r\n" +
                      "Subject: " + message.subject + "\r\n" +
                      "MIME-Version: 1.0\r\n" +
                      "Content-Type: text/plain; charset=UTF-8\r\n" +
                      "Content-Transfer-Encoding: 8bit\r\n" +
                      "\r\n" + message.body;

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      error = "curl_easy_init failed";
      return false;
    }
    curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, message.to.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from_.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, &read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &context);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                     static_cast<curl_off_t>(context.payload.size()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    if (starttls_) {
      curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
      error = std::string("smtp send failed: ") + curl_easy_strerror(rc);
    }

    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
  }

 private:
  std::string url_;
  std::string from_;
  bool starttls_ = true;
};

}  // namespace

std::unique_ptr<EmailSender> make_log_email_sender() {
  return std::make_unique<LogEmailSender>();
}

std::unique_ptr<EmailSender> make_smtp_email_sender(std::string url, std::string from,
                                                    bool starttls) {
  return std::make_unique<SmtpEmailSender>(std::move(url), std::move(from), starttls);
}

}  // namespace auth
