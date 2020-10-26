#pragma once

#include <optional>

#include <yaml_config/yaml_config.hpp>

#include <server/handlers/auth/auth_checker_base.hpp>
#include <server/handlers/auth/auth_checker_factory.hpp>
#include <server/handlers/auth/auth_checker_settings.hpp>
#include <server/handlers/auth/handler_auth_config.hpp>
#include <server/http/handler_methods.hpp>

namespace server::handlers::auth::apikey {

class AuthCheckerApiKey : public AuthCheckerBase {
 public:
  AuthCheckerApiKey(const HandlerAuthConfig& auth_config,
                    const AuthCheckerSettings& settings);

  [[nodiscard]] AuthCheckResult CheckAuth(
      const http::HttpRequest& request,
      request::RequestContext& context) const override;

  [[nodiscard]] bool SupportsUserAuth() const noexcept override {
    return false;
  }

 private:
  struct ApiKeyTypeByMethodSettings {
    std::array<std::optional<std::string>, http::kHandlerMethodsMax + 1>
        apikey_type;
  };

  friend ApiKeyTypeByMethodSettings Parse(
      const yaml_config::YamlConfig& value,
      formats::parse::To<ApiKeyTypeByMethodSettings>);

  const ApiKeysSet* GetApiKeysForRequest(
      const http::HttpRequest& request) const;

  std::array<const ApiKeysSet*, http::kHandlerMethodsMax + 1> keys_by_method_{};
};

}  // namespace server::handlers::auth::apikey
