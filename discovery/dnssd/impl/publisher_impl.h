// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DISCOVERY_DNSSD_IMPL_PUBLISHER_IMPL_H_
#define DISCOVERY_DNSSD_IMPL_PUBLISHER_IMPL_H_

#include <map>
#include <string>

#include "discovery/dnssd/impl/conversion_layer.h"
#include "discovery/dnssd/public/dns_sd_instance.h"
#include "discovery/dnssd/public/dns_sd_publisher.h"
#include "discovery/mdns/public/mdns_domain_confirmed_provider.h"
#include "discovery/mdns/public/mdns_service.h"

namespace openscreen::discovery {

class NetworkInterfaceConfig;
class ReportingClient;

class PublisherImpl : public DnsSdPublisher,
                      public MdnsDomainConfirmedProvider {
 public:
  PublisherImpl(MdnsService& publisher,
                ReportingClient& reporting_client,
                TaskRunner& task_runner,
                const NetworkInterfaceConfig& network_config);
  ~PublisherImpl() override;

  // DnsSdPublisher overrides.
  Error Register(const DnsSdInstance& instance, Client* client) override;
  Error UpdateRegistration(const DnsSdInstance& instance) override;
  ErrorOr<int> DeregisterAll(const std::string& service) override;

 private:
  Error UpdatePublishedRegistration(const DnsSdInstance& instance);

  // MdnsDomainConfirmedProvider overrides.
  void OnDomainFound(const DomainName& requested_name,
                     const DomainName& confirmed_name) override;

  // The set of instances which will be published once the mDNS Probe phase
  // completes.
  std::map<DnsSdInstance, Client* const> pending_instances_;

  // Maps from the requested instance to the endpoint which was published after
  // the mDNS Probe phase was completed. The only difference between these
  // instances should be the instance name.
  std::map<DnsSdInstance, DnsSdInstanceEndpoint> published_instances_;

  MdnsService& mdns_publisher_;
  ReportingClient& reporting_client_;
  TaskRunner& task_runner_;
  const NetworkInterfaceConfig& network_config_;

  friend class PublisherTesting;
};

}  // namespace openscreen::discovery

#endif  // DISCOVERY_DNSSD_IMPL_PUBLISHER_IMPL_H_
