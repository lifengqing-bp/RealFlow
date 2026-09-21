#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace byteturn {

struct SentenceSegmenterConfig {
  std::size_t min_characters = 12;
  std::size_t target_characters = 60;
  std::size_t hard_max_characters = 140;
};

class SentenceSegmenter {
 public:
  explicit SentenceSegmenter(SentenceSegmenterConfig config = {});
  std::vector<std::string> push(const std::string& text_delta);
  std::vector<std::string> flush();
  void reset();

 private:
  std::vector<std::string> extract(bool flushing);
  std::size_t character_count(std::size_t bytes) const;
  SentenceSegmenterConfig config_;
  std::string buffer_;
};

}  // namespace byteturn
