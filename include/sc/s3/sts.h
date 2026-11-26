#pragma once

#include <sc/s3/settings.h>
#include <sc/s3/signer.h>

#include <chrono>
#include <string>
#include <string_view>

namespace sc::s3 {
    /// What to ask STS's AssumeRole for.
    struct assume_role_options {
        /// "arn:aws:iam::123456789012:role/backup-upload" (an S3-compatible server may need none).
        std::string role_arn;
        /// Names the session in CloudTrail ("backup-db3-20261123"); 2 to 64 of A-Z a-z 0-9 _ = , . @ -.
        std::string session_name = "sc-s3";
        /// 15 minutes to the role's maximum (1 hour unless the role allows more).
        std::chrono::seconds duration{3600};
        /// A session policy (JSON) narrowing what the credentials may do below the role's own:
        /// one bucket, one prefix, PutObject only.
        std::string policy;
        /// The role's external id, when its trust policy asks for one.
        std::string external_id;
    };

    /// Temporary credentials for a role (STS AssumeRole), asked for with the settings' credentials
    /// and signed for the settings' region: a key id, a secret and a session token that expire.
    /// Throws error for a refusal (AccessDenied, ...) or no answer.
    ///
    ///     auto settings = sc::s3::settings::load();
    ///     auto upload_only = sc::s3::assume_role(settings, {.role_arn = "arn:aws:iam::123456789012:role/upload",
    ///                                                      .session_name = "backup-hv2", .duration = std::chrono::minutes{30}});
    ///     // handed to the host doing the upload, in its environment only
    [[nodiscard]] credentials assume_role(const settings &settings, const assume_role_options &options);

    /// Reads AssumeRole's answer; throws std::runtime_error when it holds no credentials.
    [[nodiscard]] credentials parse_assume_role(std::string_view xml);

    /// One call to an AWS service that speaks the query protocol (STS, CloudWatch, IAM, ...; since
    /// 2.1.0): form POSTed to url ("https://monitoring.af-south-1.amazonaws.com"), signed for
    /// settings' region and service ("monitoring"), tried again settings.retries times when it's
    /// worth it. Returns the answer's XML; throws error with AWS's code otherwise. what names the
    /// call in errors ("CloudWatch GetMetricData").
    [[nodiscard]] std::string query(const settings &settings, std::string_view service, std::string_view url, const signed_request::fields &form,
                                    std::string_view what);
}
