/*
 * flow_steering.hpp - Flow steering helper for DPA packet mirroring
 */

#pragma once

#include <infiniband/mlx5dv.h>
#include <cstdint>
#include <memory>

namespace dpa {

struct FlowMatcher {
    struct mlx5dv_dr_domain *dr_domain{nullptr};
    struct mlx5dv_dr_table *dr_table_root{nullptr};
    struct mlx5dv_dr_matcher *dr_matcher_root{nullptr};
    struct mlx5dv_dr_table *dr_table_sws{nullptr};
    struct mlx5dv_dr_matcher *dr_matcher_sws{nullptr};

    ~FlowMatcher();
};

struct FlowRule {
    struct mlx5dv_dr_action *action{nullptr};
    struct mlx5dv_dr_rule *dr_rule{nullptr};

    ~FlowRule();
};

class FlowSteeringManager {
public:
    FlowSteeringManager() = default;
    ~FlowSteeringManager() = default;

    // Disallow copy
    FlowSteeringManager(const FlowSteeringManager &) = delete;
    FlowSteeringManager &operator=(const FlowSteeringManager &) = delete;

    // Setup RX steering for specified UDP destination port to DPA TIR
    bool setup_rx_rule(struct ibv_context *ibv_ctx,
                       struct mlx5dv_devx_obj *tir_obj,
                       uint16_t udp_dport);

    // Setup TX steering to forward replies out of the wire (DPU mode)
    bool setup_tx_rules(struct ibv_context *ibv_ctx,
                        uint64_t client_dmac);

    void clear();

private:
    std::unique_ptr<FlowMatcher> rx_matcher_;
    std::unique_ptr<FlowRule> rx_rule_;

    std::unique_ptr<FlowMatcher> tx_matcher_;
    std::unique_ptr<FlowRule> tx_rule_table_;
    std::unique_ptr<FlowRule> tx_rule_vport_;
};

} // namespace dpa

