#include <sc/s3/settings.h>

#include <sc/core/config.h>

#include <sys/stat.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace sc::s3 {
    namespace {
        std::string env(const char *name) {
            const char *value = std::getenv(name);
            return value ? value : "";
        }

        std::string text_of(const nlohmann::ordered_json &value, const std::string &name) {
            if (value.is_string()) return value.get<std::string>();
            if (value.is_number() || value.is_boolean()) return value.dump();
            throw std::runtime_error(name + ": not a value");
        }

        bool flag(const nlohmann::ordered_json &value, const std::string &name) {
            if (value.is_boolean()) return value.get<bool>();
            const auto text = text_of(value, name);
            if (text == "true" || text == "yes" || text == "on" || text == "1") return true;
            if (text == "false" || text == "no" || text == "off" || text == "0") return false;
            throw std::runtime_error(name + ": not true or false: " + text);
        }

        int whole(const nlohmann::ordered_json &value, const std::string &name, const int least) {
            int number = 0;
            try {
                number = value.is_number_integer() ? value.get<int>() : std::stoi(text_of(value, name));
            } catch (const std::logic_error &) {
                throw std::runtime_error(name + ": not a whole number");
            }
            if (number < least) throw std::runtime_error(name + ": at least " + std::to_string(least));
            return number;
        }

        // Others may read a file holding a secret: say so (but go on).
        void warn_if_readable(const std::filesystem::path &file) {
            struct stat info{};
            if (stat(file.c_str(), &info) == 0 && (info.st_mode & S_IROTH)) {
                std::cerr << "sc-s3: warning: " << file.string() << " holds a secret key and everyone can read it (chmod 600 or 640)" << std::endl;
            }
        }

        void read_values(settings &out, const nlohmann::ordered_json &values, const std::filesystem::path &file) {
            for (const auto &[name, value]: values.items()) {
                if (name == "region") out.region = text_of(value, name);
                else if (name == "endpoint") out.endpoint = text_of(value, name);
                else if (name == "path_style") out.path_style = flag(value, name);
                else if (name == "sts_endpoint") out.sts_endpoint = text_of(value, name);
                else if (name == "access_key_id") out.credentials.access_key_id = text_of(value, name);
                else if (name == "secret_access_key") out.credentials.secret_access_key = text_of(value, name);
                else if (name == "session_token") out.credentials.session_token = text_of(value, name);
                else if (name == "rate_limit") {
                    try {
                        out.rate_limit = bytes::parse(text_of(value, name));
                    } catch (const std::invalid_argument &failure) {
                        throw std::runtime_error(std::string{"rate_limit: "} + failure.what());
                    }
                } else if (name == "retries") out.retries = whole(value, name, 0);
                else if (name == "connect_timeout") out.connect_timeout = std::chrono::seconds{whole(value, name, 1)};
                else if (name == "idle_timeout") out.idle_timeout = std::chrono::seconds{whole(value, name, 1)};
                else if (name == "ca_file") out.ca_file = text_of(value, name);
                else if (name == "verify_tls") out.verify_tls = flag(value, name);
                else if (name == "s3" && value.is_object()) read_values(out, value, file); // the same keys in an [s3] section
                else throw std::runtime_error("unknown setting " + name);
            }
        }

        void read_file(settings &out, const std::filesystem::path &file) {
            const sc::config conf{file};
            read_values(out, conf, file);
            if (!out.credentials.secret_access_key.empty()) warn_if_readable(file);
        }
    }

    settings settings::load(const std::filesystem::path &file, const bool environment) {
        settings out;
        const bool is_default = file == std::filesystem::path{default_file};
        if (!file.empty()) {
            if (std::filesystem::exists(file)) {
                try {
                    read_file(out, file);
                } catch (const std::runtime_error &failure) {
                    throw std::runtime_error(file.string() + ": " + failure.what());
                }
            } else if (!is_default) {
                throw std::runtime_error(file.string() + ": no such file");
            }
        }
        if (!environment) return out;

        if (const auto region = env("AWS_REGION"); !region.empty()) out.region = region;
        else if (const auto fallback = env("AWS_DEFAULT_REGION"); !fallback.empty()) out.region = fallback;
        if (const auto endpoint = env("AWS_ENDPOINT_URL_S3"); !endpoint.empty()) out.endpoint = endpoint;
        else if (const auto general = env("AWS_ENDPOINT_URL"); !general.empty()) out.endpoint = general;
        if (const auto sts = env("AWS_ENDPOINT_URL_STS"); !sts.empty()) out.sts_endpoint = sts;
        // A key from the environment replaces the file's whole, never mixed with it.
        if (const auto id = env("AWS_ACCESS_KEY_ID"), secret = env("AWS_SECRET_ACCESS_KEY"); !id.empty() || !secret.empty()) {
            if (id.empty() || secret.empty()) throw std::runtime_error("AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY go together");
            out.credentials = {id, secret, env("AWS_SESSION_TOKEN"), std::nullopt};
        }
        return out;
    }

    namespace {
        std::string without_slash(std::string url) {
            while (!url.empty() && url.back() == '/') url.pop_back();
            return url;
        }
    }

    std::string settings::s3_url() const {
        return endpoint.empty() ? "https://s3." + region + ".amazonaws.com" : without_slash(endpoint);
    }

    std::string settings::sts_url() const {
        if (!sts_endpoint.empty()) return without_slash(sts_endpoint);
        if (!endpoint.empty()) return without_slash(endpoint);
        return "https://sts." + region + ".amazonaws.com";
    }
}
