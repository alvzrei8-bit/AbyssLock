#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Byte = unsigned char;

struct Options {
  std::string input;
  std::string output;
  std::uint64_t seed = 0;
  bool explicit_seed = false;
  bool mangle = true;
  bool anti_debug = true;
};

struct Token {
  enum class Kind { Word, Number, String, Symbol };
  Kind kind;
  std::string text;
};

struct Random {
  std::uint64_t state;

  explicit Random(std::uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}

  std::uint32_t next() {
    state ^= state << 7;
    state ^= state >> 9;
    state ^= state << 8;
    return static_cast<std::uint32_t>(state >> 16);
  }

  std::uint32_t range(std::uint32_t low, std::uint32_t high) {
    return low + next() % (high - low + 1);
  }
};

std::string read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("unable to open input file: " + path);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void write_file(const std::string& path, const std::string& value) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("unable to open output file: " + path);
  }
  file.write(value.data(), static_cast<std::streamsize>(value.size()));
  if (!file) {
    throw std::runtime_error("unable to write output file: " + path);
  }
}

bool is_word_start(char value) {
  return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
}

bool is_word_char(char value) {
  return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

bool is_keyword(std::string_view value) {
  static const std::unordered_set<std::string> keywords = {
      "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
      "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
      "true", "until", "while", "continue", "type", "export", "declare", "where",
      "self", "and", "or", "not"};
  return keywords.contains(std::string(value));
}

std::vector<Token> lex(std::string_view source) {
  std::vector<Token> tokens;
  std::size_t index = 0;
  while (index < source.size()) {
    const char current = source[index];
    if (std::isspace(static_cast<unsigned char>(current))) {
      ++index;
      continue;
    }
    if (current == '-' && index + 1 < source.size() && source[index + 1] == '-') {
      index += 2;
      if (index + 1 < source.size() && source[index] == '[' && source[index + 1] == '[') {
        index += 2;
        while (index + 1 < source.size() && !(source[index] == ']' && source[index + 1] == ']')) {
          ++index;
        }
        index = std::min(source.size(), index + 2);
      } else {
        while (index < source.size() && source[index] != '\n') {
          ++index;
        }
      }
      continue;
    }
    if (current == '"' || current == '\'') {
      const char quote = current;
      const std::size_t start = index++;
      bool escaped = false;
      while (index < source.size()) {
        const char value = source[index++];
        if (escaped) {
          escaped = false;
        } else if (value == '\\') {
          escaped = true;
        } else if (value == quote) {
          break;
        }
      }
      tokens.push_back({Token::Kind::String, std::string(source.substr(start, index - start))});
      continue;
    }
    if (is_word_start(current)) {
      const std::size_t start = index++;
      while (index < source.size() && is_word_char(source[index])) {
        ++index;
      }
      tokens.push_back({Token::Kind::Word, std::string(source.substr(start, index - start))});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(current))) {
      const std::size_t start = index++;
      while (index < source.size() &&
             (std::isalnum(static_cast<unsigned char>(source[index])) || source[index] == '.' ||
              source[index] == '_')) {
        ++index;
      }
      tokens.push_back({Token::Kind::Number, std::string(source.substr(start, index - start))});
      continue;
    }
    const std::size_t start = index++;
    if (index < source.size()) {
      const std::string pair(source.substr(start, 2));
      if (pair == ".." || pair == "==" || pair == "~=" || pair == "<=" || pair == ">=" ||
          pair == "::" || pair == "->" || pair == "//" || pair == "<<") {
        ++index;
      }
    }
    tokens.push_back({Token::Kind::Symbol, std::string(source.substr(start, index - start))});
  }
  return tokens;
}

void collect_local_names(const std::vector<Token>& tokens, std::unordered_set<std::string>& locals) {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != Token::Kind::Word) {
      continue;
    }
    if (tokens[i].text == "local") {
      std::size_t cursor = i + 1;
      if (cursor < tokens.size() && tokens[cursor].text == "function") {
        ++cursor;
      }
      while (cursor < tokens.size()) {
        if (tokens[cursor].kind == Token::Kind::Word) {
          locals.insert(tokens[cursor].text);
          ++cursor;
          if (cursor >= tokens.size() || tokens[cursor].text != ",") {
            break;
          }
          ++cursor;
          continue;
        }
        break;
      }
    }
    if (tokens[i].text == "for" && i + 1 < tokens.size() &&
        tokens[i + 1].kind == Token::Kind::Word) {
      locals.insert(tokens[i + 1].text);
    }
    if (tokens[i].text == "function") {
      std::size_t cursor = i + 1;
      while (cursor < tokens.size() && tokens[cursor].text != "(" &&
             tokens[cursor].text != "do" && tokens[cursor].text != "end") {
        ++cursor;
      }
      if (cursor < tokens.size() && tokens[cursor].text == "(") {
        ++cursor;
        while (cursor < tokens.size() && tokens[cursor].text != ")") {
          if (tokens[cursor].kind == Token::Kind::Word && !is_keyword(tokens[cursor].text)) {
            locals.insert(tokens[cursor].text);
          }
          ++cursor;
        }
      }
    }
  }
}

std::string base36(std::uint32_t value) {
  static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string output;
  do {
    output.push_back(digits[value % 36]);
    value /= 36;
  } while (value != 0);
  std::reverse(output.begin(), output.end());
  return output;
}

std::string mangle_source(const std::vector<Token>& tokens, std::uint64_t seed, bool enabled) {
  std::unordered_set<std::string> locals;
  if (enabled) {
    collect_local_names(tokens, locals);
  }
  std::unordered_map<std::string, std::string> replacements;
  if (enabled) {
    std::uint32_t sequence = static_cast<std::uint32_t>(seed);
    for (const auto& token : tokens) {
      if (token.kind == Token::Kind::Word && locals.contains(token.text) &&
          !replacements.contains(token.text)) {
        sequence = sequence * 1664525U + 1013904223U;
        replacements[token.text] = "_a" + base36(sequence);
      }
    }
  }
  std::string output;
  std::string previous;
  for (const auto& token : tokens) {
    std::string value = token.text;
    if (token.kind == Token::Kind::Word && replacements.contains(value)) {
      value = replacements[value];
    }
    const bool needs_space = !previous.empty() &&
        ((std::isalnum(static_cast<unsigned char>(previous.back())) || previous.back() == '_') &&
         (std::isalnum(static_cast<unsigned char>(value.front())) || value.front() == '_'));
    if (needs_space) {
      output.push_back(' ');
    }
    output += value;
    previous = value;
  }
  return output;
}

void reset_lzw(std::unordered_map<std::string, std::uint32_t>& dictionary, std::uint32_t& next) {
  dictionary.clear();
  for (std::uint32_t i = 0; i < 256; ++i) {
    dictionary[std::string(1, static_cast<char>(i))] = i;
  }
  next = 256;
}

std::vector<std::uint32_t> lzw_compress(std::string_view input) {
  constexpr std::uint32_t reset_code = 65535;
  std::unordered_map<std::string, std::uint32_t> dictionary;
  std::uint32_t next = 256;
  reset_lzw(dictionary, next);
  std::vector<std::uint32_t> codes;
  std::string current;
  for (const unsigned char value : input) {
    const std::string candidate = current + static_cast<char>(value);
    if (dictionary.contains(candidate)) {
      current = candidate;
      continue;
    }
    if (!current.empty()) {
      codes.push_back(dictionary.at(current));
    }
    if (next < reset_code) {
      dictionary[candidate] = next++;
    } else {
      codes.push_back(reset_code);
      reset_lzw(dictionary, next);
    }
    current.assign(1, static_cast<char>(value));
  }
  if (!current.empty()) {
    codes.push_back(dictionary.at(current));
  }
  return codes;
}

std::uint32_t fnv(const std::vector<std::uint32_t>& values) {
  std::uint32_t hash = 2166136261U;
  for (const std::uint32_t value : values) {
    hash ^= value;
    hash *= 16777619U;
  }
  return hash;
}

std::string escape_luau_string(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char byte : value) {
    if (byte == '\\') {
      output << "\\\\";
    } else if (byte == '"') {
      output << "\\\"";
    } else if (byte >= 32 && byte <= 126) {
      output << static_cast<char>(byte);
    } else {
      output << '\\';
      output.width(3);
      output.fill('0');
      output << static_cast<unsigned int>(byte);
    }
  }
  return output.str();
}

std::string numeric_array(const std::vector<std::uint32_t>& values) {
  std::ostringstream output;
  output << '{';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      output << ',';
    }
    output << values[i];
  }
  output << '}';
  return output.str();
}

std::string encoded_constant(const std::vector<std::uint32_t>& values, std::uint32_t mask,
                             bool string_mode) {
  if (!string_mode) {
    std::vector<std::uint32_t> encoded;
    encoded.reserve(values.size());
    for (const auto value : values) {
      encoded.push_back((value + mask) & 0xffffU);
    }
    return numeric_array(encoded);
  }
  std::string bytes;
  bytes.reserve(values.size() * 2);
  for (const auto value : values) {
    const std::uint32_t encoded = (value + mask) & 0xffffU;
    bytes.push_back(static_cast<char>(encoded & 0xffU));
    bytes.push_back(static_cast<char>((encoded >> 8U) & 0xffU));
  }
  return "\"" + escape_luau_string(bytes) + "\"";
}

std::string emit_luau(std::string_view transformed, std::uint64_t seed, bool anti_debug) {
  Random random(seed);
  const std::uint32_t op_decode = random.range(32, 220);
  std::uint32_t op_execute = random.range(32, 220);
  while (op_execute == op_decode) {
    op_execute = random.range(32, 220);
  }
  std::uint32_t op_halt = random.range(32, 220);
  while (op_halt == op_decode || op_halt == op_execute) {
    op_halt = random.range(32, 220);
  }
  const std::uint32_t op_mask = random.range(3, 241);
  const std::uint32_t data_mask = random.range(11, 60000);
  const std::uint32_t register_payload = random.range(1, 5);
  const std::uint32_t register_source = random.range(1, 5);
  const std::uint32_t register_result = random.range(1, 5);
  const bool string_mode = (random.next() & 1U) != 0;
  const bool branch_dispatch = (random.next() & 1U) != 0;
  const auto compressed = lzw_compress(transformed);
  const std::vector<std::uint32_t> encoded_values = [&]() {
    std::vector<std::uint32_t> values;
    values.reserve(compressed.size());
    for (const auto value : compressed) {
      values.push_back((value + data_mask) & 0xffffU);
    }
    return values;
  }();
  std::vector<std::uint32_t> fingerprint_values = compressed;
  fingerprint_values.insert(fingerprint_values.end(),
                            {op_decode, op_execute, op_halt, op_mask, data_mask,
                             register_payload, register_source, register_result});
  const std::uint32_t fingerprint = fnv(fingerprint_values);
  const std::uint32_t encoded_decode = (op_decode + op_mask) & 0xffU;
  const std::uint32_t encoded_execute = (op_execute + op_mask) & 0xffU;
  const std::uint32_t encoded_halt = (op_halt + op_mask) & 0xffU;
  std::ostringstream output;
  output << "local __ab=function();";
  output << "local function __x() error(\"AbyssLock integrity check failed\",0) end;";
  if (anti_debug) {
    output << "local __d=rawget(_G,\"debug\");";
    output << "if __d and type(__d.gethook)==\"function\" then local __h=__d.gethook() if __h then __x() end end;";
  }
  if (string_mode) {
    output << "local __c=" << encoded_constant(compressed, data_mask, true) << ";";
  } else {
    output << "local __c=" << encoded_constant(compressed, data_mask, false) << ";";
  }
  output << "local __m=" << data_mask << ";";
  output << "local __f=" << fingerprint << ";";
  output << "local __p=" << op_mask << ";";
  output << "local __q={" << encoded_decode << "," << encoded_execute << "," << encoded_halt << "};";
  output << "local __r={" << register_payload << "," << register_source << "," << register_result << "};";
  output << "local __v={};";
  output << "local __i=function(t)local h=2166136261 for i=1,#t do h=(h~t[i])*16777619%4294967296 end return h end;";
  if (string_mode) {
    output << "for i=1,#__c,2 do local a=string.byte(__c,i) local b=string.byte(__c,i+1) __v[#__v+1]=((a+b*256-__m)%65536) end;";
  } else {
    output << "for i=1,#__c do __v[i]=(__c[i]-__m)%65536 end;";
  }
  output << "local __z={};";
  output << "for i=0,255 do __z[i]=string.char(i) end;";
  output << "local __n=256;";
  output << "local __w=nil;";
  output << "local __o={};";
  output << "local __g=function(c);";
  output << "if c==65535 then __z={} for i=0,255 do __z[i]=string.char(i) end __n=256 __w=nil return \"\" end;";
  output << "local e=__z[c] or (__w and __w..string.sub(__w,1,1)) if not e then __x() end;";
  output << "__o[#__o+1]=e if __w then __z[__n]=__w..string.sub(e,1,1) __n=__n+1 end __w=e return e;";
  output << "end;";
  output << "local __s=nil;";
  output << "local __t=nil;";
  output << "local __h={};";
  output << "local __a=function() __s=table.concat(__o) end;";
  output << "local __b=function() if type(loadstring)~=\"function\" then __x() end __t=loadstring(__s) if type(__t)~=\"function\" then __x() end end;";
  output << "local __e=function() return true end;";
  std::vector<int> handlers = {0, 1, 2};
  std::shuffle(handlers.begin(), handlers.end(), std::mt19937(static_cast<std::uint32_t>(seed)));
  for (const int handler : handlers) {
    if (handler == 0) {
      output << "__h[" << op_decode << "]=function() for i=1,#__v do if __g(__v[i])==\"\" then end end end;";
    } else if (handler == 1) {
      output << "__h[" << op_execute << "]=__b;";
    } else {
      output << "__h[" << op_halt << "]=__e;";
    }
  }
  output << "local __u={{" << encoded_decode << ",0},{" << encoded_execute << ",0},{" << encoded_halt << ",0}};";
  output << "if __i(__v)~=" << fnv(compressed) << " then __x() end;";
  output << "local __y={__q[1],__q[2],__q[3],__p,__m,__r[1],__r[2],__r[3]};";
  output << "for i=1,#__y do __v[#__v+1]=__y[i] end;";
  output << "if __i(__v)~=" << fingerprint << " then __x() end;";
  output << "local __j=function(k)return __h[k] end;";
  if (branch_dispatch) {
    output << "__j=function(k) if k==" << op_decode << " then return __h[k] elseif k==" << op_execute
           << " then return __h[k] elseif k==" << op_halt << " then return __h[k] end return nil end;";
  }
  output << "local __k=1; while true do local __l=__u[__k] __k=__k+1 local __qv=(__l[1]-__p)%256 local __fn=__j(__qv) if not __fn then __x() end if __fn(__l) then break end end;";
  output << "return __t() end return __ab()";
  return output.str();
}

std::string obfuscate(std::string_view source, const Options& options) {
  Random random(options.seed);
  const std::uint64_t seed = options.seed ? options.seed : (static_cast<std::uint64_t>(random.next()) << 32U) | random.next();
  const std::vector<Token> tokens = lex(source);
  const std::string transformed = mangle_source(tokens, seed, options.mangle);
  return emit_luau(transformed, seed, options.anti_debug);
}

void usage() {
  std::cout << "AbyssLock Luau obfuscator\n";
  std::cout << "usage: abysslock <input.luau> [-o output.luau] [--seed N] [--no-mangle] [--no-anti-debug]\n";
}

Options parse_options(int argc, char** argv) {
  Options options;
  if (argc < 2) {
    usage();
    throw std::runtime_error("missing input file");
  }
  if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
    usage();
    std::exit(0);
  }
  options.input = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "-o" && i + 1 < argc) {
      options.output = argv[++i];
    } else if (argument == "--seed" && i + 1 < argc) {
      options.seed = std::stoull(argv[++i]);
      options.explicit_seed = true;
    } else if (argument == "--no-mangle") {
      options.mangle = false;
    } else if (argument == "--no-anti-debug") {
      options.anti_debug = false;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + argument);
    }
  }
  if (options.output.empty()) {
    options.output = options.input + ".obfuscated.luau";
  }
  if (!options.explicit_seed) {
    std::random_device source;
    options.seed = (static_cast<std::uint64_t>(source()) << 32U) | source();
  }
  return options;
}

}

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const std::string source = read_file(options.input);
    const std::string result = obfuscate(source, options);
    write_file(options.output, result);
    std::cout << "AbyssLock wrote " << options.output << " using seed " << options.seed << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "AbyssLock: " << error.what() << "\n";
    return 1;
  }
}