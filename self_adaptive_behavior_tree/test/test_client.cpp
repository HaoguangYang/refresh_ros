#include <rclcpp/executors.hpp>
#include <rclcpp/rclcpp.hpp>

#include "refresh_ros_msgs/action/sleep.hpp"
#include "self_adaptive_behavior_tree/bt_action_node.hpp"

using namespace BT;

//-------------------------------------------------------------
// Simple Action to print a number
//-------------------------------------------------------------

class PrintValue : public BT::SyncActionNode {
 public:
  PrintValue(const std::string& name, const BT::NodeConfiguration& config)
      : BT::SyncActionNode(name, config) {}

  BT::NodeStatus tick() override {
    std::string msg;
    if (getInput("message", msg)) {
      std::cout << "PrintValue: " << msg << std::endl;
      return NodeStatus::SUCCESS;
    } else {
      std::cout << "PrintValue FAILED " << std::endl;
      return NodeStatus::FAILURE;
    }
  }

  static BT::PortsList providedPorts() { return {BT::InputPort<std::string>("message")}; }
};

class SleepAction : public RosActionNode<refresh_ros_msgs::action::Sleep> {
 public:
  SleepAction(const std::string& name, const BT::NodeConfiguration& conf,
              const RosActionNodeParams& params,
              typename std::shared_ptr<ActionClient> action_client)
      : RosActionNode<refresh_ros_msgs::action::Sleep>(name, conf, params, action_client) {}

  static BT::PortsList providedPorts() { return {InputPort<unsigned>("msec")}; }

  bool sendGoal(Goal& goal) override {
    auto timeout = getInput<unsigned>("msec");
    goal.msec_timeout = timeout.value();
    return true;
  }

  BT::NodeStatus onResult(const WrappedResult& wr) override {
    RCLCPP_INFO(node_->get_logger(), "onResultReceived %d", wr.result->done);
    return wr.result->done ? NodeStatus::SUCCESS : NodeStatus::FAILURE;
  }

  virtual BT::NodeStatus onFailure(const ActionNodeErrorCode& error) override {
    RCLCPP_ERROR(node_->get_logger(), "onFailure %d", error);
    return NodeStatus::FAILURE;
  }
};

//-----------------------------------------------------

// Simple tree, used to execute once each action.
static const char* xml_text = R"(
 <root >
     <BehaviorTree>
        <Sequence>
            <PrintValue message="start"/>
            <Sleep msec="2000"/>
            <PrintValue message="sleep completed"/>
            <Fallback>
                <Timeout msec="500">
                   <Sleep msec="1000"/>
                </Timeout>
                <PrintValue message="sleep aborted"/>
            </Fallback>
        </Sequence>
     </BehaviorTree>
 </root>
 )";

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto nh = std::make_shared<rclcpp::Node>("sleep_client");

  BehaviorTreeFactory factory;

  factory.registerNodeType<PrintValue>("PrintValue");

  RosActionNodeParams params = RosActionNodeParams(nh, "sleep_service", 2000);
  RegisterRosAction<SleepAction>(factory, "Sleep", params);

  auto tree = factory.createTreeFromText(xml_text);

  NodeStatus status = NodeStatus::IDLE;

  while (rclcpp::ok()) {
    status = tree.tickRoot();
    tree.sleep(std::chrono::milliseconds(100));
  }

  return 0;
}
