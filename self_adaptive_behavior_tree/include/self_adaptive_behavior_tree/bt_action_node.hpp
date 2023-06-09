#ifndef BEHAVIOR_TREE_ROS2__BT_ACTION_NODE_HPP_
#define BEHAVIOR_TREE_ROS2__BT_ACTION_NODE_HPP_

#include <memory>
#include <string>

// ROS 2 headers
#include <rclcpp/allocator/allocator_common.hpp>
#include <rclcpp/executors.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

#include "behaviortree_cpp_v3/action_node.h"
#include "behaviortree_cpp_v3/bt_factory.h"

namespace BT {

struct RosActionNodeParams {
  std::shared_ptr<rclcpp::Node> nh;
  std::string action_name;
  std::chrono::milliseconds server_timeout;

  RosActionNodeParams(const std::shared_ptr<rclcpp::Node>& node,
                      const std::string& action_name = "", const unsigned int& server_timeout = 0)
      : nh(node), action_name(action_name), server_timeout(server_timeout){};
};

enum class ActionNodeErrorCode {
  SERVER_UNREACHABLE,
  SEND_GOAL_TIMEOUT,
  GOAL_REJECTED_BY_SERVER,
  ACTION_ABORTED,
  ACTION_CANCELLED,
  INVALID_GOAL
};

/**
 * @brief Abstract class representing use to call a ROS2 Action (client).
 *
 * It will try to be non-blocking for the entire duration of the call.
 * The derived class whould reimplement the virtual methods as described below.
 */
template <class ActionT>
class RosActionNode : public ActionNodeBase {
 public:
  // Type definitions
  using ActionType = ActionT;
  using ActionClient = typename rclcpp_action::Client<ActionT>;
  using Goal = typename ActionT::Goal;
  using GoalHandle = typename rclcpp_action::ClientGoalHandle<ActionT>;
  using WrappedResult = typename rclcpp_action::ClientGoalHandle<ActionT>::WrappedResult;
  using Feedback = typename ActionT::Feedback;
  using FeedbackConstPtr = typename ActionT::Feedback::ConstPtr;
  using Params = RosActionNodeParams;
  using Error = ActionNodeErrorCode;

  /** You are not supposed to instantiate this class directly, the factory will do it.
   * To register this class into the factory, use:
   *
   *    RegisterRosAction<DerivedClasss>(factory, params)
   *
   * Note that if the external_action_client is not set, the constructor will build its own.
   * */
  explicit RosActionNode(const std::string& instance_name, const NodeConfiguration& conf,
                         const RosActionNodeParams& params,
                         typename std::shared_ptr<ActionClient> external_action_client = {});

  virtual ~RosActionNode() = default;

  /// These ports will be added automatically if this Node is
  /// registered using RegisterRosAction<DeriveClass>()
  static PortsList providedPorts() {
    return {InputPort<std::string>("action_name", "", "name of the Action Server"),
            InputPort<unsigned>("timeout", 0, "timeout to connect (milliseconds)"),
            OutputPort<Feedback>("feedback", "pointer to action feedback")};
  }

  NodeStatus tick() override final;

  /// The default halt() implementation will call cancelGoal is necessary.
  void halt() override;

  /** sendGoal is a callback invoked to return the goal message (ActionT::Goal).
   * If conditions are not met, it should return "false" and the Action
   * will return FAILURE.
   */
  virtual bool sendGoal(Goal& goal) = 0;

  /** Callback invoked when the result is received by the server.
   * It is up to the user to define if the action returns SUCCESS or FAILURE.
   */
  virtual NodeStatus onResult(const WrappedResult& result) {
    setStatus(NodeStatus::SUCCESS);
    std::unique_ptr<Feedback> fb_res = resultToFeedback(result);
    if (fb_res != nullptr) setOutput("feedback", *fb_res);
    return NodeStatus::SUCCESS;
  };

  /** Callback invoked when the feedback is received.
   * It generally returns RUNNING, but the user can also use this callback to cancel the
   * current action and return SUCCESS or FAILURE.
   */
  virtual NodeStatus onFeeback(const FeedbackConstPtr feedback) {
    setOutput("feedback", *feedback);
    return NodeStatus::RUNNING;
  }

  /**
   * @brief Some action servers do not provide any feedbacks, even when the result has reached. This
   * function allows users to generate one feedback based on the result of the action.
   *
   * @param res Action result returned by the action server.
   * @return std::unique_ptr<Feedback> Generated feedback. Default to a null-pointer.
   */
  virtual std::unique_ptr<Feedback> resultToFeedback(const WrappedResult& res) {
    (void)res;
    return nullptr;
  }

  /** Callback invoked when something goes wrong.
   * It must return either SUCCESS or FAILURE.
   */
  virtual NodeStatus onFailure(const Error& error) {
    (void)error;
    return NodeStatus::FAILURE;
  }

  /// Method used to send a request to the Action server to cancel the current goal
  void cancelGoal();

 protected:
  std::shared_ptr<rclcpp::Node> node_;
  const std::string action_name_;
  const std::chrono::milliseconds server_timeout_;

 private:
  NodeStatus checkStatus(const NodeStatus& status) {
    if (status != NodeStatus::SUCCESS && status != NodeStatus::FAILURE) {
      throw std::logic_error("RosActionNode: the callback must return either SUCCESS of FAILURE");
    }
    return status;
  };

  typename std::shared_ptr<ActionClient> action_client_;
  rclcpp::CallbackGroup::SharedPtr callback_group_;

  std::shared_future<typename GoalHandle::SharedPtr> future_goal_handle_;
  typename GoalHandle::SharedPtr goal_handle_;

  rclcpp::Time time_goal_sent_;
  NodeStatus on_feedback_state_change_;
  bool goal_received_;
  WrappedResult result_;
};

/// Method to register the service into a factory.
/// It gives you the opportunity to set the ros::NodeHandle.
template <class DerivedT>
static void RegisterRosAction(
    BehaviorTreeFactory& factory, const std::string& registration_ID,
    const RosActionNodeParams& params,
    std::shared_ptr<typename DerivedT::ActionClient> external_client = {}) {
  NodeBuilder builder = [=](const std::string& name, const NodeConfiguration& config) {
    return std::make_unique<DerivedT>(name, config, params, external_client);
  };

  TreeNodeManifest manifest;
  manifest.type = getType<DerivedT>();
  manifest.ports = DerivedT::providedPorts();
  manifest.registration_ID = registration_ID;
  const auto& basic_ports = DerivedT::providedPorts();
  manifest.ports.insert(basic_ports.begin(), basic_ports.end());
  factory.registerBuilder(manifest, builder);
}

//----------------------------------------------------------------
//---------------------- DEFINITIONS -----------------------------
//----------------------------------------------------------------

template <class T>
inline RosActionNode<T>::RosActionNode(
    const std::string& instance_name, const NodeConfiguration& conf,
    const RosActionNodeParams& params,
    typename std::shared_ptr<ActionClient> external_action_client)
    : ActionNodeBase(instance_name, conf),
      node_(params.nh),
      action_name_(params.action_name),
      server_timeout_(params.server_timeout) {
  if (external_action_client) {
    action_client_ = external_action_client;
  } else {
    callback_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    action_client_ = nullptr;
  }
}

template <class T>
inline NodeStatus RosActionNode<T>::tick() {
  // first step to be done only at the beginning of the Action
  if (status() == NodeStatus::IDLE) {
    if (action_client_ == nullptr) {
      std::string action_name = "";
      Result inRes;
      if (!(inRes = getInput<std::string>("action_name", action_name))) action_name = action_name_;
      if (action_name.length() == 0) action_name = action_name_;
      unsigned server_timeout = 0;
      if (!(inRes = getInput<unsigned>("timeout", server_timeout)))
        server_timeout = server_timeout_.count();
      if (server_timeout == 0) server_timeout = server_timeout_.count();
      action_client_ = rclcpp_action::create_client<T>(node_, action_name, callback_group_);
      bool found =
          action_client_->wait_for_action_server(std::chrono::milliseconds(server_timeout));
      if (!found) {
        RCLCPP_ERROR(node_->get_logger(), "Action server [%s] is not reachable.",
                     action_name.c_str());
        return checkStatus(onFailure(Error::SERVER_UNREACHABLE));
      }
    }

    setStatus(NodeStatus::RUNNING);

    goal_received_ = false;
    future_goal_handle_ = {};
    on_feedback_state_change_ = NodeStatus::RUNNING;
    result_ = {};

    Goal goal;

    if (!sendGoal(goal)) {
      return checkStatus(onFailure(Error::INVALID_GOAL));
    }

    typename ActionClient::SendGoalOptions goal_options;

    //--------------------
    goal_options.feedback_callback = [this](typename GoalHandle::SharedPtr,
                                            const std::shared_ptr<const Feedback> feedback) {
      on_feedback_state_change_ = onFeeback(feedback);
      if (on_feedback_state_change_ == NodeStatus::IDLE) {
        throw std::logic_error("onFeeback must not retunr IDLE");
      }
      emitStateChanged();
    };
    //--------------------
    goal_options.result_callback = [this](const WrappedResult& result) {
      RCLCPP_INFO(node_->get_logger(), "result_callback");
      result_ = result;
      emitStateChanged();
    };
    //--------------------
    goal_options.goal_response_callback = [this](const std::shared_ptr<GoalHandle>& goal_handle) {
      if (goal_handle == nullptr) {
        RCLCPP_ERROR(node_->get_logger(), "Goal was rejected by server");
        setStatus(checkStatus(onFailure(Error::GOAL_REJECTED_BY_SERVER)));
        emitStateChanged();
      } else {
        RCLCPP_INFO(node_->get_logger(), "Goal accepted by server, waiting for result");
      }
    };
    //--------------------

    future_goal_handle_ = action_client_->async_send_goal(goal, goal_options);
    time_goal_sent_ = node_->now();

    return NodeStatus::RUNNING;
  }

  if (status() == NodeStatus::RUNNING) {
    rclcpp::spin_some(node_);

    // FIRST case: check if the goal request has a timeout
    if (!goal_received_) {
      auto nodelay = std::chrono::milliseconds(0);
      auto timeout = rclcpp::Duration::from_seconds(double(server_timeout_.count()) / 1000);

      if (rclcpp::spin_until_future_complete(node_, future_goal_handle_, nodelay) !=
          rclcpp::FutureReturnCode::SUCCESS) {
        RCLCPP_WARN(node_->get_logger(), "waiting goal confirmation");
        if ((node_->now() - time_goal_sent_) > timeout) {
          RCLCPP_WARN(node_->get_logger(), "TIMEOUT");
          return checkStatus(onFailure(Error::SEND_GOAL_TIMEOUT));
        } else {
          return NodeStatus::RUNNING;
        }
      } else {
        goal_received_ = true;
        goal_handle_ = future_goal_handle_.get();
        future_goal_handle_ = {};

        if (!goal_handle_) {
          return checkStatus(onFailure(Error::GOAL_REJECTED_BY_SERVER));
          // throw std::runtime_error("Goal was rejected by the action server");
        }
      }
    }

    // SECOND case: onFeeback requested a stop
    if (on_feedback_state_change_ != NodeStatus::RUNNING) {
      cancelGoal();
      return on_feedback_state_change_;
    }
    // THIRD case: result received, requested a stop
    if (result_.code != rclcpp_action::ResultCode::UNKNOWN) {
      if (result_.code == rclcpp_action::ResultCode::ABORTED) {
        return checkStatus(onFailure(Error::ACTION_ABORTED));
      } else if (result_.code == rclcpp_action::ResultCode::CANCELED) {
        return checkStatus(onFailure(Error::ACTION_CANCELLED));
      } else {
        return checkStatus(onResult(result_));
      }
    }
  }
  return NodeStatus::RUNNING;
}

template <class T>
inline void RosActionNode<T>::halt() {
  if (status() == NodeStatus::RUNNING) {
    cancelGoal();
  }
}

template <class T>
inline void RosActionNode<T>::cancelGoal() {
  auto future_cancel = action_client_->async_cancel_goal(goal_handle_);

  if (rclcpp::spin_until_future_complete(node_, future_cancel, server_timeout_) !=
      rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(node_->get_logger(), "Failed to cancel action server for %s",
                 action_name_.c_str());
  }
}

template <class ActionT>
class ActionEvaluatorNode : public StatefulActionNode {
 protected:
  ActionEvaluatorNode(const std::string& name, const NodeConfiguration& conf)
      : StatefulActionNode(name, conf) {}

 public:
  using ActionType = ActionT;
  using Feedback = typename ActionT::Feedback;

  ActionEvaluatorNode() = delete;

  virtual ~ActionEvaluatorNode() = default;

  /// These ports will be added automatically if this Node is
  /// registered using RegisterReFRESH_EV<DeriveClass>()
  static PortsList providedPorts() {
    return {InputPort<Feedback>("feedback"), OutputPort<float>("performance_cost"),
            OutputPort<float>("resource_cost")};
  }

  virtual NodeStatus spinOnce() = 0;

  inline NodeStatus spinOnceImpl() {
    Result fbRes;
    if (!(fbRes = getInput<Feedback>("feedback", fb_)))
      throw(
          RuntimeError("Action Evaluator Node missing required input [feedback]: ", fbRes.error()));
    NodeStatus status = spinOnce();
    setOutput("performance_cost", pCost_);
    setOutput("resource_cost", rCost_);
    setStatus(status);
    return status;
  }

  inline NodeStatus onStart() override {
    setStatus(NodeStatus::RUNNING);
    return spinOnceImpl();
  }

  /// method invoked by an action in the RUNNING state.
  inline NodeStatus onRunning() override { return spinOnceImpl(); }

  inline void onHalted() override {
    // TODO: what to do here?
    return;
  }

 protected:
  Feedback fb_;
  float pCost_, rCost_;
};

/// Method to register the evaluator into a factory.
template <class DerivedT>
static void RegisterActionEvaluator(BehaviorTreeFactory& factory,
                                    const std::string& registration_ID) {
  NodeBuilder builder = [](const std::string& name, const NodeConfiguration& config) {
    return std::make_unique<DerivedT>(name, config);
  };

  TreeNodeManifest manifest;
  manifest.type = getType<DerivedT>();
  manifest.ports = DerivedT::providedPorts();
  manifest.registration_ID = registration_ID;
  const auto& basic_ports = ActionEvaluatorNode<typename DerivedT::ActionType>::providedPorts();
  manifest.ports.insert(basic_ports.begin(), basic_ports.end());

  factory.registerBuilder(manifest, builder);
}

}  // namespace BT

#endif  // BEHAVIOR_TREE_ROS2__BT_ACTION_NODE_HPP_
