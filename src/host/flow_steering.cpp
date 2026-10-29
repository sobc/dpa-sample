/*
 * flow_steering.cpp - Implementation of Flow Steering Manager for DPA
 */

#include "flow_steering.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

namespace dpa {

// Matcher criteria flags for Direct Rules
enum matcher_criteria {
  MATCHER_CRITERIA_EMPTY = 0,
  MATCHER_CRITERIA_OUTER = 1 << 0,
  MATCHER_CRITERIA_MISC = 1 << 1,
  MATCHER_CRITERIA_INNER = 1 << 2,
  MATCHER_CRITERIA_MISC2 = 1 << 3,
  MATCHER_CRITERIA_MISC3 = 1 << 4,
};

// PRM Outer match specification layout (64 bytes)
struct mlx5_ifc_dr_match_spec_bits {
  uint8_t smac_47_16[0x20];
  uint8_t smac_15_0[0x10];
  uint8_t ethertype[0x10];

  uint8_t dmac_47_16[0x20];
  uint8_t dmac_15_0[0x10];
  uint8_t first_prio[0x3];
  uint8_t first_cfi[0x1];
  uint8_t first_vid[0xc];

  uint8_t ip_protocol[0x8];
  uint8_t ip_dscp[0x6];
  uint8_t ip_ecn[0x2];
  uint8_t cvlan_tag[0x1];
  uint8_t svlan_tag[0x1];
  uint8_t frag[0x1];
  uint8_t ip_version[0x4];
  uint8_t tcp_flags[0x9];

  uint8_t tcp_sport[0x10];
  uint8_t tcp_dport[0x10];

  uint8_t reserved_at_c0[0x18];
  uint8_t ip_ttl_hoplimit[0x8];

  uint8_t udp_sport[0x10];
  uint8_t udp_dport[0x10];

  uint8_t src_ip_127_96[0x20];
  uint8_t src_ip_95_64[0x20];
  uint8_t src_ip_63_32[0x20];
  uint8_t src_ip_31_0[0x20];

  uint8_t dst_ip_127_96[0x20];
  uint8_t dst_ip_95_64[0x20];
  uint8_t dst_ip_63_32[0x20];
  uint8_t dst_ip_31_0[0x20];
};

static constexpr size_t MATCH_VAL_BSIZE = 64;

FlowMatcher::~FlowMatcher() {
  if (dr_matcher_root) {
    mlx5dv_dr_matcher_destroy(dr_matcher_root);
  }
  if (dr_table_root) {
    mlx5dv_dr_table_destroy(dr_table_root);
  }
  if (dr_matcher_sws) {
    mlx5dv_dr_matcher_destroy(dr_matcher_sws);
  }
  if (dr_table_sws) {
    mlx5dv_dr_table_destroy(dr_table_sws);
  }
  if (dr_domain) {
    mlx5dv_dr_domain_destroy(dr_domain);
  }
}

FlowRule::~FlowRule() {
  if (dr_rule) {
    mlx5dv_dr_rule_destroy(dr_rule);
  }
  if (action) {
    mlx5dv_dr_action_destroy(action);
  }
}

void FlowSteeringManager::clear() {
  // Rules must be destroyed before matchers
  rx_rule_.reset();
  rx_matcher_.reset();
  tx_rule_vport_.reset();
  tx_rule_table_.reset();
  tx_matcher_.reset();
}

bool FlowSteeringManager::setup_rx_rule(struct ibv_context *ibv_ctx,
                                        struct mlx5dv_devx_obj *tir_obj,
                                        uint16_t udp_dport) {
  rx_matcher_ = std::make_unique<FlowMatcher>();

  // 1. Create NIC_RX domain
  rx_matcher_->dr_domain =
      mlx5dv_dr_domain_create(ibv_ctx, MLX5DV_DR_DOMAIN_TYPE_NIC_RX);
  if (!rx_matcher_->dr_domain) {
    std::cerr << "[FlowSteering] Failed to create NIC_RX dr_domain (errno: "
              << errno << ")\n";
    return false;
  }

  // 2. Create root table
  rx_matcher_->dr_table_root =
      mlx5dv_dr_table_create(rx_matcher_->dr_domain, 0);
  if (!rx_matcher_->dr_table_root) {
    std::cerr << "[FlowSteering] Failed to create root dr_table (errno: "
              << errno << ")\n";
    return false;
  }

  // 3. Create matcher matching on IPv4 UDP destination port
  size_t match_params_size =
      sizeof(struct mlx5dv_flow_match_parameters) + MATCH_VAL_BSIZE;
  auto match_mask = static_cast<struct mlx5dv_flow_match_parameters *>(
      std::calloc(1, match_params_size));
  if (!match_mask) {
    return false;
  }
  match_mask->match_sz = MATCH_VAL_BSIZE;

  // Match criteria: EtherType = IPv4, IP protocol = UDP, UDP dst port
  DEVX_SET(dr_match_spec, match_mask->match_buf, ethertype, 0xFFFF);
  DEVX_SET(dr_match_spec, match_mask->match_buf, ip_protocol, 0xFF);
  DEVX_SET(dr_match_spec, match_mask->match_buf, udp_dport, 0xFFFF);

  rx_matcher_->dr_matcher_root = mlx5dv_dr_matcher_create(
      rx_matcher_->dr_table_root, 0, MATCHER_CRITERIA_OUTER, match_mask);
  std::free(match_mask);

  if (!rx_matcher_->dr_matcher_root) {
    std::cerr << "[FlowSteering] Failed to create RX dr_matcher (errno: "
              << errno << ")\n";
    return false;
  }

  // 4. Create rule directing matched packets to the DPA TIR
  auto match_value = static_cast<struct mlx5dv_flow_match_parameters *>(
      std::calloc(1, match_params_size));
  if (!match_value) {
    return false;
  }
  match_value->match_sz = MATCH_VAL_BSIZE;

  DEVX_SET(dr_match_spec, match_value->match_buf, ethertype, 0x0800);
  DEVX_SET(dr_match_spec, match_value->match_buf, ip_protocol, 0x11);
  DEVX_SET(dr_match_spec, match_value->match_buf, udp_dport, udp_dport);

  rx_rule_ = std::make_unique<FlowRule>();
  rx_rule_->action = mlx5dv_dr_action_create_dest_devx_tir(tir_obj);
  if (!rx_rule_->action) {
    std::cerr << "[FlowSteering] Failed to create TIR action (errno: " << errno
              << ")\n";
    std::free(match_value);
    return false;
  }

  struct mlx5dv_dr_action *actions[1] = {rx_rule_->action};
  rx_rule_->dr_rule = mlx5dv_dr_rule_create(rx_matcher_->dr_matcher_root,
                                            match_value, 1, actions);
  std::free(match_value);

  if (!rx_rule_->dr_rule) {
    std::cerr << "[FlowSteering] Failed to create RX dr_rule (errno: " << errno
              << ")\n";
    return false;
  }

  return true;
}

bool FlowSteeringManager::setup_tx_rules(struct ibv_context *ibv_ctx,
                                        uint64_t client_dmac)
{
  tx_matcher_ = std::make_unique<FlowMatcher>();

  // 1. Create FDB domain for TX steering in DPU mode
  tx_matcher_->dr_domain =
      mlx5dv_dr_domain_create(ibv_ctx, MLX5DV_DR_DOMAIN_TYPE_FDB);
  if (!tx_matcher_->dr_domain) {
    std::cerr << "[FlowSteering] Failed to create FDB dr_domain (errno: "
              << errno << ")\n";
    return false;
  }

  tx_matcher_->dr_table_root =
      mlx5dv_dr_table_create(tx_matcher_->dr_domain, 0);
  if (!tx_matcher_->dr_table_root) {
    std::cerr << "[FlowSteering] Failed to create TX root table (errno: "
              << errno << ")\n";
    return false;
  }

  size_t match_params_size =
      sizeof(struct mlx5dv_flow_match_parameters) + MATCH_VAL_BSIZE;
  auto match_mask = static_cast<struct mlx5dv_flow_match_parameters *>(
      std::calloc(1, match_params_size));
  if (!match_mask) {
    return false;
  }
  match_mask->match_sz = MATCH_VAL_BSIZE;

  // Match criteria: Destination MAC (as required by BlueField hardware FDB)
  DEVX_SET(dr_match_spec, match_mask->match_buf, dmac_47_16, 0xFFFFFFFF);
  DEVX_SET(dr_match_spec, match_mask->match_buf, dmac_15_0, 0xFFFF);

  tx_matcher_->dr_matcher_root = mlx5dv_dr_matcher_create(
      tx_matcher_->dr_table_root, 0, MATCHER_CRITERIA_OUTER, match_mask);

  tx_matcher_->dr_table_sws = mlx5dv_dr_table_create(tx_matcher_->dr_domain, 1);
    if (!tx_matcher_->dr_table_sws) {
      std::cerr << "[FlowSteering] Failed to create TX dr_table_sws (errno: "
                << errno << ")\n";
      std::free(match_mask);
      return false;
    }

    tx_matcher_->dr_matcher_sws = mlx5dv_dr_matcher_create(
        tx_matcher_->dr_table_sws, 0, MATCHER_CRITERIA_OUTER, match_mask);
    std::free(match_mask);

    if (!tx_matcher_->dr_matcher_root || !tx_matcher_->dr_matcher_sws) {
      std::cerr << "[FlowSteering] Failed to create TX matchers (errno: "
                << errno << ")\n";
      return false;
    }

    // Value to match
    auto match_value = static_cast<struct mlx5dv_flow_match_parameters *>(
        std::calloc(1, match_params_size));
    if (!match_value) {
      return false;
    }
    match_value->match_sz = MATCH_VAL_BSIZE;
    DEVX_SET(dr_match_spec, match_value->match_buf, dmac_47_16,
             client_dmac >> 16);
    DEVX_SET(dr_match_spec, match_value->match_buf, dmac_15_0,
             client_dmac & 0xFFFF);

    // Rule 1: Forward root table to SWS table
    tx_rule_table_ = std::make_unique<FlowRule>();
    tx_rule_table_->action =
        mlx5dv_dr_action_create_dest_table(tx_matcher_->dr_table_sws);
    if (!tx_rule_table_->action) {
      std::free(match_value);
      return false;
    }
    struct mlx5dv_dr_action *act_table[1] = {tx_rule_table_->action};
    tx_rule_table_->dr_rule = mlx5dv_dr_rule_create(
        tx_matcher_->dr_matcher_root, match_value, 1, act_table);

    // Rule 2: Forward from SWS table to wire (vport 0xFFFF)
    tx_rule_vport_ = std::make_unique<FlowRule>();
    tx_rule_vport_->action =
        mlx5dv_dr_action_create_dest_vport(tx_matcher_->dr_domain, 0xFFFF);
    if (!tx_rule_vport_->action) {
      std::free(match_value);
      return false;
    }
    struct mlx5dv_dr_action *act_vport[1] = {tx_rule_vport_->action};
    tx_rule_vport_->dr_rule = mlx5dv_dr_rule_create(tx_matcher_->dr_matcher_sws,
                                                    match_value, 1, act_vport);

    std::free(match_value);
    return (tx_rule_table_->dr_rule && tx_rule_vport_->dr_rule);
  }

} // namespace dpa
