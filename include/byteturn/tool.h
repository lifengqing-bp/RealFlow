#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace byteturn {

using ToolHandler = std::function<std::string(const std::string& arguments)>;

class ToolRegistry {
 public:
  bool add(std::string name, ToolHandler handler);
  std::string invoke(const std::string& name, const std::string& arguments) const;

 private:
  std::unordered_map<std::string, ToolHandler> tools_;
};

}  // namespace byteturn

