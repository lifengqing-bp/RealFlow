#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace byteturn {

using ToolHandler = std::function<std::string(const std::string& arguments)>;
using CancellableToolHandler = std::function<std::string(
    const std::string& arguments, const std::function<bool()>& stop_requested)>;

class ToolRegistry {
 public:
  bool add(std::string name, ToolHandler handler);
  bool add(std::string name, CancellableToolHandler handler);
  std::string invoke(const std::string& name, const std::string& arguments) const;
  std::string invoke(const std::string& name, const std::string& arguments,
                     const std::function<bool()>& stop_requested) const;

 private:
  std::unordered_map<std::string, CancellableToolHandler> tools_;
};

}  // namespace byteturn
