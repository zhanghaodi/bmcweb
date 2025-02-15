/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include "error_messages.hpp"
#include "openbmc_dbus_rest.hpp"
#include "redfish_util.hpp"

#include <app.hpp>
#include <registries/privilege_registry.hpp>
#include <utils/json_utils.hpp>

#include <optional>
#include <variant>
namespace redfish
{

void getNTPProtocolEnabled(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp);
std::string getHostName();

const static std::array<std::pair<std::string, std::string>, 3> protocolToDBus{
    {{"SSH", "dropbear"}, {"HTTPS", "bmcweb"}, {"IPMI", "phosphor-ipmi-net"}}};

inline void
    extractNTPServersAndDomainNamesData(const GetManagedObjects& dbusData,
                                        std::vector<std::string>& ntpData,
                                        std::vector<std::string>& dnData)
{
    for (const auto& obj : dbusData)
    {
        for (const auto& ifacePair : obj.second)
        {
            if (obj.first == "/xyz/openbmc_project/network/eth0")
            {
                if (ifacePair.first ==
                    "xyz.openbmc_project.Network.EthernetInterface")
                {
                    for (const auto& propertyPair : ifacePair.second)
                    {
                        if (propertyPair.first == "NTPServers")
                        {
                            const std::vector<std::string>* ntpServers =
                                std::get_if<std::vector<std::string>>(
                                    &propertyPair.second);
                            if (ntpServers != nullptr)
                            {
                                ntpData = *ntpServers;
                            }
                        }
                        else if (propertyPair.first == "DomainName")
                        {
                            const std::vector<std::string>* domainNames =
                                std::get_if<std::vector<std::string>>(
                                    &propertyPair.second);
                            if (domainNames != nullptr)
                            {
                                dnData = *domainNames;
                            }
                        }
                    }
                }
            }
        }
    }
}

template <typename CallbackFunc>
void getEthernetIfaceData(CallbackFunc&& callback)
{
    crow::connections::systemBus->async_method_call(
        [callback{std::move(callback)}](
            const boost::system::error_code errorCode,
            const GetManagedObjects& dbusData) {
            std::vector<std::string> ntpServers;
            std::vector<std::string> domainNames;

            if (errorCode)
            {
                callback(false, ntpServers, domainNames);
                return;
            }

            extractNTPServersAndDomainNamesData(dbusData, ntpServers,
                                                domainNames);

            callback(true, ntpServers, domainNames);
        },
        "xyz.openbmc_project.Network", "/xyz/openbmc_project/network",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

inline void getNetworkData(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const crow::Request& req)
{
    asyncResp->res.jsonValue["@odata.type"] =
        "#ManagerNetworkProtocol.v1_5_0.ManagerNetworkProtocol";
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/Managers/bmc/NetworkProtocol";
    asyncResp->res.jsonValue["Id"] = "NetworkProtocol";
    asyncResp->res.jsonValue["Name"] = "Manager Network Protocol";
    asyncResp->res.jsonValue["Description"] = "Manager Network Service";
    asyncResp->res.jsonValue["Status"]["Health"] = "OK";
    asyncResp->res.jsonValue["Status"]["HealthRollup"] = "OK";
    asyncResp->res.jsonValue["Status"]["State"] = "Enabled";

    // HTTP is Mandatory attribute as per OCP Baseline Profile - v1.0.0,
    // but from security perspective it is not recommended to use.
    // Hence using protocolEnabled as false to make it OCP and security-wise
    // compliant
    asyncResp->res.jsonValue["HTTP"]["Port"] = 0;
    asyncResp->res.jsonValue["HTTP"]["ProtocolEnabled"] = false;

    for (auto& protocol : protocolToDBus)
    {
        asyncResp->res.jsonValue[protocol.first]["Port"] =
            nlohmann::detail::value_t::null;
        asyncResp->res.jsonValue[protocol.first]["ProtocolEnabled"] = false;
    }

    std::string hostName = getHostName();

    asyncResp->res.jsonValue["HostName"] = hostName;

    getNTPProtocolEnabled(asyncResp);

    // TODO Get eth0 interface data, and call the below callback for JSON
    // preparation
    getEthernetIfaceData(
        [hostName, asyncResp](const bool& success,
                              const std::vector<std::string>& ntpServers,
                              const std::vector<std::string>& domainNames) {
            if (!success)
            {
                messages::resourceNotFound(asyncResp->res, "EthernetInterface",
                                           "eth0");
                return;
            }
            asyncResp->res.jsonValue["NTP"]["NTPServers"] = ntpServers;
            if (hostName.empty() == false)
            {
                std::string fqdn = hostName;
                if (domainNames.empty() == false)
                {
                    fqdn += ".";
                    fqdn += domainNames[0];
                }
                asyncResp->res.jsonValue["FQDN"] = std::move(fqdn);
            }
        });

    Privileges effectiveUserPrivileges =
        redfish::getUserPrivileges(req.userRole);

    // /redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates is
    // something only ConfigureManager can access then only display when
    // the user has permissions ConfigureManager
    if (isOperationAllowedWithPrivileges({{"ConfigureManager"}},
                                         effectiveUserPrivileges))
    {
        asyncResp->res.jsonValue["HTTPS"]["Certificates"] = {
            {"@odata.id",
             "/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates"}};
    }

    for (const auto& protocol : protocolToDBus)
    {
        const std::string& protocolName = protocol.first;
        const std::string& serviceName = protocol.second;
        getPortStatusAndPath(
            serviceName,
            [asyncResp, protocolName](const boost::system::error_code ec,
                                      const std::string& socketPath,
                                      bool isProtocolEnabled) {
                if (ec)
                {
                    messages::internalError(asyncResp->res);
                    return;
                }
                asyncResp->res.jsonValue[protocolName]["ProtocolEnabled"] =
                    isProtocolEnabled;
                getPortNumber(
                    socketPath,
                    [asyncResp, protocolName](
                        const boost::system::error_code ec, int portNumber) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        asyncResp->res.jsonValue[protocolName]["Port"] =
                            portNumber;
                    });
            });
    }
} // namespace redfish

#ifdef BMCWEB_ALLOW_DEPRECATED_HOSTNAME_PATCH
inline void
    handleHostnamePatch(const std::string& hostName,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec) {
            if (ec)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        },
        "xyz.openbmc_project.Network", "/xyz/openbmc_project/network/config",
        "org.freedesktop.DBus.Properties", "Set",
        "xyz.openbmc_project.Network.SystemConfiguration", "HostName",
        std::variant<std::string>(hostName));
}
#endif

inline void handleNTPProtocolEnabled(
    const bool& ntpEnabled, const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    std::string timeSyncMethod;
    if (ntpEnabled)
    {
        timeSyncMethod = "xyz.openbmc_project.Time.Synchronization.Method.NTP";
    }
    else
    {
        timeSyncMethod =
            "xyz.openbmc_project.Time.Synchronization.Method.Manual";
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code errorCode) {
            if (errorCode)
            {
                messages::internalError(asyncResp->res);
            }
        },
        "xyz.openbmc_project.Settings", "/xyz/openbmc_project/time/sync_method",
        "org.freedesktop.DBus.Properties", "Set",
        "xyz.openbmc_project.Time.Synchronization", "TimeSyncMethod",
        std::variant<std::string>{timeSyncMethod});
}

inline void
    handleNTPServersPatch(const std::vector<std::string>& ntpServers,
                          const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec) {
            if (ec)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        },
        "xyz.openbmc_project.Network", "/xyz/openbmc_project/network/eth0",
        "org.freedesktop.DBus.Properties", "Set",
        "xyz.openbmc_project.Network.EthernetInterface", "NTPServers",
        std::variant<std::vector<std::string>>{ntpServers});
}

inline void
    handleProtocolEnabled(const bool protocolEnabled,
                          const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string_view netBasePath)
{
    crow::connections::systemBus->async_method_call(
        [protocolEnabled, asyncResp,
         netBasePath](const boost::system::error_code ec,
                      const crow::openbmc_mapper::GetSubTreeType& subtree) {
            if (ec)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            for (const auto& entry : subtree)
            {
                if (boost::algorithm::starts_with(entry.first, netBasePath))
                {
                    crow::connections::systemBus->async_method_call(
                        [asyncResp](const boost::system::error_code ec2) {
                            if (ec2)
                            {
                                messages::internalError(asyncResp->res);
                                return;
                            }
                        },
                        entry.second.begin()->first, entry.first,
                        "org.freedesktop.DBus.Properties", "Set",
                        "xyz.openbmc_project.Control.Service.Attributes",
                        "Running", std::variant<bool>{protocolEnabled});

                    crow::connections::systemBus->async_method_call(
                        [asyncResp](const boost::system::error_code ec2) {
                            if (ec2)
                            {
                                messages::internalError(asyncResp->res);
                                return;
                            }
                        },
                        entry.second.begin()->first, entry.first,
                        "org.freedesktop.DBus.Properties", "Set",
                        "xyz.openbmc_project.Control.Service.Attributes",
                        "Enabled", std::variant<bool>{protocolEnabled});
                }
            }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree",
        "/xyz/openbmc_project/control/service", 0,
        std::array<const char*, 1>{
            "xyz.openbmc_project.Control.Service.Attributes"});
}

inline std::string getHostName()
{
    std::string hostName;

    std::array<char, HOST_NAME_MAX> hostNameCStr;
    if (gethostname(hostNameCStr.data(), hostNameCStr.size()) == 0)
    {
        hostName = hostNameCStr.data();
    }
    return hostName;
}

inline void
    getNTPProtocolEnabled(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code errorCode,
                    const std::variant<std::string>& timeSyncMethod) {
            if (errorCode)
            {
                return;
            }

            const std::string* s = std::get_if<std::string>(&timeSyncMethod);

            if (*s == "xyz.openbmc_project.Time.Synchronization.Method.NTP")
            {
                asyncResp->res.jsonValue["NTP"]["ProtocolEnabled"] = true;
            }
            else if (*s == "xyz.openbmc_project.Time.Synchronization."
                           "Method.Manual")
            {
                asyncResp->res.jsonValue["NTP"]["ProtocolEnabled"] = false;
            }
        },
        "xyz.openbmc_project.Settings", "/xyz/openbmc_project/time/sync_method",
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.Time.Synchronization", "TimeSyncMethod");
}

inline void requestRoutesNetworkProtocol(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/NetworkProtocol/")
        .privileges(redfish::privileges::patchManagerNetworkProtocol)
        .methods(boost::beast::http::verb::patch)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                std::optional<std::string> newHostName;
                std::optional<nlohmann::json> ntp;
                std::optional<nlohmann::json> ipmi;
                std::optional<nlohmann::json> ssh;

                if (!json_util::readJson(req, asyncResp->res, "NTP", ntp,
                                         "HostName", newHostName, "IPMI", ipmi,
                                         "SSH", ssh))
                {
                    return;
                }

                asyncResp->res.result(boost::beast::http::status::no_content);
                if (newHostName)
                {
#ifdef BMCWEB_ALLOW_DEPRECATED_HOSTNAME_PATCH
                    handleHostnamePatch(*newHostName, asyncResp);
#else
                    messages::propertyNotWritable(asyncResp->res, "HostName");
#endif
                }

                if (ntp)
                {
                    std::optional<std::vector<std::string>> ntpServers;
                    std::optional<bool> ntpEnabled;
                    if (!json_util::readJson(*ntp, asyncResp->res, "NTPServers",
                                             ntpServers, "ProtocolEnabled",
                                             ntpEnabled))
                    {
                        return;
                    }

                    if (ntpEnabled)
                    {
                        handleNTPProtocolEnabled(*ntpEnabled, asyncResp);
                    }

                    if (ntpServers)
                    {
                        std::sort((*ntpServers).begin(), (*ntpServers).end());
                        (*ntpServers)
                            .erase(std::unique((*ntpServers).begin(),
                                               (*ntpServers).end()),
                                   (*ntpServers).end());
                        handleNTPServersPatch(*ntpServers, asyncResp);
                    }
                }

                if (ipmi)
                {
                    std::optional<bool> ipmiProtocolEnabled;
                    if (!json_util::readJson(*ipmi, asyncResp->res,
                                             "ProtocolEnabled",
                                             ipmiProtocolEnabled))
                    {
                        return;
                    }

                    if (ipmiProtocolEnabled)
                    {
                        handleProtocolEnabled(
                            *ipmiProtocolEnabled, asyncResp,
                            "/xyz/openbmc_project/control/service/"
                            "phosphor_2dipmi_2dnet_40");
                    }
                }

                if (ssh)
                {
                    std::optional<bool> sshProtocolEnabled;
                    if (!json_util::readJson(*ssh, asyncResp->res,
                                             "ProtocolEnabled",
                                             sshProtocolEnabled))
                    {
                        return;
                    }

                    if (sshProtocolEnabled)
                    {
                        handleProtocolEnabled(
                            *sshProtocolEnabled, asyncResp,
                            "/xyz/openbmc_project/control/service/dropbear");
                    }
                }
            });

    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/NetworkProtocol/")
        .privileges(redfish::privileges::getManagerNetworkProtocol)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                getNetworkData(asyncResp, req);
            });
}

} // namespace redfish
