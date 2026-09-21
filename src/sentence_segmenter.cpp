#include "byteturn/sentence_segmenter.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace byteturn {
namespace {
std::string trim(std::string text) {
  const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c);
  });
  const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c);
  }).base();
  return first < last ? std::string(first, last) : std::string{};
}
bool starts_at(const std::string& value, std::size_t position, const char* token) {
  return value.compare(position, std::char_traits<char>::length(token), token) == 0;
}
}  // namespace

SentenceSegmenter::SentenceSegmenter(SentenceSegmenterConfig config)
    : config_(config) {
  if (config_.min_characters == 0 ||
      config_.target_characters < config_.min_characters ||
      config_.hard_max_characters < config_.target_characters)
    throw std::invalid_argument("invalid sentence segmenter limits");
}

std::vector<std::string> SentenceSegmenter::push(const std::string& text_delta) {
  buffer_ += text_delta;
  return extract(false);
}
std::vector<std::string> SentenceSegmenter::flush() { return extract(true); }
void SentenceSegmenter::reset() { buffer_.clear(); }

std::size_t SentenceSegmenter::character_count(std::size_t bytes) const {
  std::size_t count = 0;
  for (std::size_t i = 0; i < std::min(bytes, buffer_.size()); ++i)
    if ((static_cast<unsigned char>(buffer_[i]) & 0xc0) != 0x80) ++count;
  return count;
}

std::vector<std::string> SentenceSegmenter::extract(bool flushing) {
  std::vector<std::string> output;
  while (!buffer_.empty()) {
    std::size_t boundary = std::string::npos;
    std::size_t soft_boundary = std::string::npos;
    for (std::size_t i = 0; i < buffer_.size(); ++i) {
      std::size_t punctuation_bytes = 0;
      bool strong = false;
      if (buffer_[i] == '!' || buffer_[i] == '?' || buffer_[i] == '\n') {
        punctuation_bytes = 1;
        strong = true;
      } else if (starts_at(buffer_, i, "。") || starts_at(buffer_, i, "！") ||
                 starts_at(buffer_, i, "？")) {
        punctuation_bytes = 3;
        strong = true;
      } else if (buffer_[i] == ';' || buffer_[i] == ',') {
        punctuation_bytes = 1;
      } else if (starts_at(buffer_, i, "；") || starts_at(buffer_, i, "，")) {
        punctuation_bytes = 3;
      } else if (buffer_[i] == '.' && i + 1 < buffer_.size() &&
                 std::isspace(static_cast<unsigned char>(buffer_[i + 1]))) {
        punctuation_bytes = 1;
        strong = true;
      }
      if (!punctuation_bytes) continue;
      const std::size_t end = i + punctuation_bytes;
      const auto characters = character_count(end);
      if (strong && characters >= config_.min_characters) {
        boundary = end;
        break;
      }
      if (characters >= config_.target_characters) soft_boundary = end;
    }
    if (boundary == std::string::npos) boundary = soft_boundary;
    if (boundary == std::string::npos &&
        character_count(buffer_.size()) >= config_.hard_max_characters) {
      std::size_t characters = 0;
      std::size_t last_space = std::string::npos;
      for (std::size_t i = 0; i < buffer_.size(); ++i) {
        if ((static_cast<unsigned char>(buffer_[i]) & 0xc0) != 0x80) ++characters;
        if (std::isspace(static_cast<unsigned char>(buffer_[i]))) last_space = i + 1;
        if (characters >= config_.target_characters) {
          boundary = last_space == std::string::npos ? i + 1 : last_space;
          break;
        }
      }
    }
    if (boundary == std::string::npos) {
      if (flushing) boundary = buffer_.size();
      else break;
    }
    std::string sentence = trim(buffer_.substr(0, boundary));
    buffer_.erase(0, boundary);
    while (!buffer_.empty() && std::isspace(static_cast<unsigned char>(buffer_.front())))
      buffer_.erase(buffer_.begin());
    if (!sentence.empty()) output.push_back(std::move(sentence));
  }
  return output;
}

}  // namespace byteturn
