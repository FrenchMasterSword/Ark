#include <Ark/Parser/Lexer.hpp>

#include <Ark/Utils.hpp>
#include <Ark/Log.hpp>

namespace Ark::internal
{
    Lexer::Lexer(bool debug) :
        m_debug(debug)
    {}

    void Lexer::feed(const std::string& code)
    {
        std::string src = code;
        std::size_t line = 1, character = 0;

        while (!src.empty())
        {
            for (auto& item: lex_regexes)
            {
                TokenType type = item.first;
                std::smatch m;

                if (std::regex_search(src, m, item.second, std::regex_constants::match_continuous))
                {
                    std::string result = m[0];

                    if (type == TokenType::Mismatch)
                    {
                        // we must check by hand if we parsed a ()[]{}, since it's not working on Windows
                        if (result == "(" || result == ")" || result == "[" || result == "]" || result == "{" || result == "}")
                        {
                            m_tokens.emplace_back(TokenType::Grouping, result, line, character);
                            src = src.substr(1);
                            break;
                        }
                        else
                            throwTokenizingError("couldn't tokenize", result, line, character);
                    }

                    if (type == TokenType::Capture || type == TokenType::GetField)
                        result = result.substr(1);  // remove the '&' / '.'

                    if (type == TokenType::Identifier && isKeyword(result))
                        type = TokenType::Keyword;

                    // stripping blanks characters between instructions, and comments
                    if (type != TokenType::Skip && type != TokenType::Comment)
                        m_tokens.emplace_back(type, result, line, character);

                    // line-char counter
                    if (std::string::npos != result.find_first_of("\r\n"))
                    {
                        line += std::count(result.begin(), result.end(), '\n');
                        character = 0;
                    }
                    auto linefeed_pos = result.find_last_of("\r\n");
                    character += result.substr(linefeed_pos != std::string::npos ? linefeed_pos : 0).size();

                    src = std::regex_replace(src, item.second, std::string(""), std::regex_constants::format_first_only);
                    break;
                }
            }
        }
    }

    const std::vector<Token>& Lexer::tokens()
    {
        return m_tokens;
    }
}
