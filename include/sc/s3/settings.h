#pragma once

#include <sc/s3/bytes.h>
#include <sc/s3/signer.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace sc::s3 {
    /// Where and how a client reaches S3, and as whom.
    ///
    ///     auto settings = sc::s3::settings::load();          // /etc/sc-s3/sc-s3.conf if there, then AWS_* variables
    ///     settings.rate_limit = sc::s3::data::MiB(10);       // a second, each way
    ///     sc::s3::client s3{settings};
    struct settings {
        /// The region requests are signed for and AWS's endpoints are named after.
        std::string region = "us-east-1";
        /// An S3-compatible server's base URL ("http://127.0.0.1:9000"); empty for AWS
        /// (https://s3.<region>.amazonaws.com).
        std::string endpoint;
        /// Buckets in the path (endpoint/bucket/key) rather than the host name (bucket.endpoint/key).
        /// Most S3-compatible servers want it; AWS takes either, and a bucket with a dot in its
        /// name always goes in the path, as its TLS certificate wouldn't match.
        bool path_style = false;
        /// STS's URL for assume_role(); empty for AWS's in the region (https://sts.<region>.amazonaws.com),
        /// or the endpoint when there is one.
        std::string sts_endpoint;

        sc::s3::credentials credentials;

        /// The most bytes a second each request sends and receives; 0 for no limit. Uploads go one
        /// part at a time, so it limits the whole upload.
        bytes rate_limit{};
        /// How often a request that failed for a reason worth retrying (no connection, a timeout,
        /// a 5xx, SlowDown) is tried again, waiting 1, 2, 4 ... up to 30 seconds in between.
        int retries = 5;
        std::chrono::seconds connect_timeout{10};
        /// Gives up on a request that moves no data for this long, however long the whole may take.
        std::chrono::seconds idle_timeout{120};
        /// A PEM file of the authorities to trust instead of the system's; for a private server.
        std::string ca_file;
        /// Whether TLS certificates and host names are checked (always, unless told otherwise).
        bool verify_tls = true;

        /// The default configuration file.
        static constexpr const char *default_file = "/etc/sc-s3/sc-s3.conf";

        /// Settings from file (if it's there; a file named other than the default must exist) and
        /// its .d drop-ins, then the environment over them (unless environment is false):
        ///
        ///     region = af-south-1                        AWS_REGION, AWS_DEFAULT_REGION
        ///     endpoint = http://127.0.0.1:9000           AWS_ENDPOINT_URL_S3, AWS_ENDPOINT_URL
        ///     path_style = true
        ///     sts_endpoint = ...                         AWS_ENDPOINT_URL_STS
        ///     access_key_id = AKIA...                    AWS_ACCESS_KEY_ID
        ///     secret_access_key = ...                    AWS_SECRET_ACCESS_KEY
        ///     session_token = ...                        AWS_SESSION_TOKEN
        ///     rate_limit = 10M
        ///     retries = 5
        ///     connect_timeout = 10                       seconds
        ///     idle_timeout = 120                         seconds
        ///     ca_file = /etc/ssl/private-ca.pem
        ///     verify_tls = true
        ///
        /// The keys may also be in an [s3] section. Credentials from the environment replace the
        /// file's whole (a key id and secret, with or without a token), so a job handed temporary
        /// ones never mixes them with a long-lived key. A file holding a secret that others can
        /// read gets a warning on std::cerr. Throws std::runtime_error for a value that isn't valid.
        static settings load(const std::filesystem::path &file = default_file, bool environment = true);

        /// The S3 endpoint requests go to: endpoint, or AWS's for the region.
        [[nodiscard]] std::string s3_url() const;
        /// The STS endpoint assume_role() posts to.
        [[nodiscard]] std::string sts_url() const;
    };
}
