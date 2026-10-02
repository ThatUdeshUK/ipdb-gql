#pragma once

#include "duckdb/common/common.hpp"

#include <regex>

namespace duckdb {

//   {{variable}} / {{variable.property}}  prompt input
//   {name TYPE}  prompt output

struct GqlLlmInput {
	string name;
	string variable;
	string property;
};

struct GqlLlmOutput {
	string name;
	string type;
};

inline vector<GqlLlmInput> GqlLlmInputs(const string &prompt) {
	static const std::regex input_regex(R"(\{\{([A-Za-z_][A-Za-z0-9_]*)(\.[A-Za-z_][A-Za-z0-9_]*)?\}\})");
	vector<GqlLlmInput> result;
	for (auto it = std::sregex_iterator(prompt.begin(), prompt.end(), input_regex); it != std::sregex_iterator();
	     ++it) {
		GqlLlmInput input;
		input.variable = (*it)[1].str();
		if ((*it)[2].matched) {
			input.property = (*it)[2].str().substr(1);
			input.name = input.variable + "." + input.property;
		} else {
			input.name = input.variable;
		}
		result.push_back(std::move(input));
	}
	return result;
}

inline vector<GqlLlmOutput> GqlLlmOutputs(const string &prompt) {
	static const std::regex output_regex(R"((\w+)\s+(INTEGER|VARCHAR|BOOLEAN|BOOL|DOUBLE))", std::regex_constants::icase);
	vector<GqlLlmOutput> result;
	for (auto it = std::sregex_iterator(prompt.begin(), prompt.end(), output_regex); it != std::sregex_iterator();
	     ++it) {
		result.push_back({(*it)[1].str(), (*it)[2].str()});
	}
	return result;
}

} // namespace duckdb
