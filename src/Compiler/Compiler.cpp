#include <Ark/Compiler/Compiler.hpp>

#include <fstream>

#include <Ark/Log.hpp>
#include <Ark/Lang/Lib.hpp>
#include <Ark/Function.hpp>

namespace Ark
{
    namespace Compiler
    {
        Compiler::Compiler(bool debug) :
            m_debug(debug)
        {}

        Compiler::~Compiler()
        {}

        void Compiler::feed(const std::string& code)
        {
            m_parser.feed(code);
            
            if (!m_parser.check())
            {
                Ark::Log::error("[Compiler] Program has errors");
                exit(1);
            }
        }

        void Compiler::compile()
        {
            /*
                Generating headers:
                    - lang name (to be sure we are executing an Ark file)
                        on 4 bytes (ark + padding)
                    - symbols table header
                        + elements
                    - values table header
                        + elements
            */
            m_bytecode.push_back('a');
            m_bytecode.push_back('r');
            m_bytecode.push_back('k');
            m_bytecode.push_back(Instruction::NOP);

            // symbols table
            m_bytecode.push_back(Instruction::SYM_TABLE_START);
                // gather symbols, values, and start to create code segments
                m_code_pages.emplace_back();  // create empty page
                _compile(m_parser.ast(), 0);
            // push size
            pushNumber(static_cast<uint16_t>(m_symbols.size()));
            // push elements
            for (auto sym : m_symbols)
            {
                // push the string, nul terminated
                for (std::size_t i=0; i < sym.size(); ++i)
                    m_bytecode.push_back(sym[i]);
                m_bytecode.push_back(Instruction::NOP);
            }

            // values table
            m_bytecode.push_back(Instruction::VAL_TABLE_START);
            // push size
            pushNumber(static_cast<uint16_t>(m_values.size()));
            // push elements (separated with 0x00)
            for (auto val : m_values)
            {
                if (val.type == ValueType::Number)
                {
                    m_bytecode.push_back(Instruction::NUMBER_TYPE);
                    auto n = std::get<dozerg::HugeNumber>(val.value);
                    std::string t = n.toString(/* base */ 16);
                    for (std::size_t i=0; i < t.size(); ++i)
                        m_bytecode.push_back(t[i]);
                }
                else if (val.type == ValueType::String)
                {
                    m_bytecode.push_back(Instruction::STRING_TYPE);
                    std::string t = std::get<std::string>(val.value);
                    for (std::size_t i=0; i < t.size(); ++i)
                        m_bytecode.push_back(t[i]);
                }
                else if (val.type == ValueType::PageAddr)
                {
                    m_bytecode.push_back(Instruction::FUNC_TYPE);
                    pushNumber(static_cast<uint16_t>(std::get<std::size_t>(val.value)));
                }

                m_bytecode.push_back(Instruction::NOP);
            }

            // start code segments
            for (auto page : m_code_pages)
            {
                m_bytecode.push_back(CODE_SEGMENT_START);
                // push number of elements
                if (!page.size())
                {
                    pushNumber(0x00);
                    return;
                }
                pushNumber(static_cast<uint16_t>(page.size()));

                for (auto inst : page)
                    m_bytecode.push_back(inst.inst);
                // just in case we got too far, always add a HALT to be sure the
                // VM won't do anything crazy
                m_bytecode.push_back(Instruction::HALT);
            }

            if (!m_code_pages.size())
            {
                m_bytecode.push_back(Instruction::CODE_SEGMENT_START);
                pushNumber(0x00);
            }
        }

        void Compiler::saveTo(const std::string& file)
        {
            std::ofstream output(file, std::ofstream::binary);
            output.write((char*) &m_bytecode[0], m_bytecode.size() * sizeof(uint8_t));
            output.close();
        }

        void Compiler::_compile(Node x, int p)
        {
            if (m_debug)
                Ark::Log::info(x);
            
            // register symbols
            if (x.nodeType() == NodeType::Symbol)
            {
                std::string name = x.getStringVal();

                auto it = std::find(builtins.begin(), builtins.end(), name);
                // check if 'name' isn't a builtin function name before pushing it as a 'var-use'
                if (it == builtins.end())
                {
                    std::size_t i = addSymbol(name);

                    page(p).emplace_back(Instruction::LOAD_SYMBOL);
                    pushNumber(static_cast<uint16_t>(i), &page(p));
                }
                else
                {
                    page(p).emplace_back(Instruction::BUILTIN);
                    pushNumber(static_cast<uint16_t>(std::distance(builtins.begin(), it)), &page(p));
                }

                return;
            }
            // register values
            if (x.nodeType() == NodeType::String || x.nodeType() == NodeType::Number)
            {
                std::size_t i = addValue(x);

                page(p).emplace_back(Instruction::LOAD_CONST);
                pushNumber(static_cast<uint16_t>(i), &page(p));

                return;
            }
            // empty code block
            if (x.list().empty())
            {
                page(p).emplace_back(Instruction::NOP);
                return;
            }
            // registering structures
            if (x.list()[0].nodeType() == NodeType::Keyword)
            {
                Keyword n = x.list()[0].keyword();

                if (n == Keyword::If)
                {
                    // compile condition
                    _compile(x.list()[1], p);
                    // jump only if needed to the x.list()[2] part
                    page(p).emplace_back(Instruction::POP_JUMP_IF_TRUE);
                        // else code, generated in a temporary page
                        m_temp_pages.emplace_back();
                        _compile(x.list()[3], -static_cast<int>(m_temp_pages.size()));
                    // relative address to jump to if condition is true, casted as unsigned (don't worry, it's normal)
                    pushNumber(static_cast<uint16_t>(m_temp_pages.back().size()), &page(p));
                    // adding temp page into current one, and removing temp page
                    for (auto&& inst : m_temp_pages.back())
                        page(p).emplace_back(inst);
                    m_temp_pages.pop_back();
                        // if code
                        _compile(x.list()[2], p);
                }
                else if (n == Keyword::Set)
                {
                    std::string name = x.list()[1].getStringVal();
                    std::size_t i = addSymbol(name);

                    // put value before symbol id
                    _compile(x.list()[2], p);

                    page(p).emplace_back(Instruction::STORE);
                    pushNumber(static_cast<uint16_t>(i), &page(p));
                }
                else if (n == Keyword::Def)
                {
                    std::string name = x.list()[1].getStringVal();
                    std::size_t i = addSymbol(name);

                    // put value before symbol id
                    _compile(x.list()[2], p);

                    page(p).emplace_back(Instruction::LET);
                    pushNumber(static_cast<uint16_t>(i), &page(p));
                }
                else if (n == Keyword::Fun)
                {
                    // create new page for function body
                    m_code_pages.emplace_back();
                    std::size_t page_id = m_code_pages.size() - 1;
                    // load value on the stack
                    page(p).emplace_back(Instruction::LOAD_CONST);
                    std::size_t id = addValue(page_id);  // save page_id into the constants table as PageAddr
                    pushNumber(static_cast<uint16_t>(id), &page(p));
                    // create a new environment for function
                    m_code_pages.back().emplace_back(Instruction::NEW_ENV);
                    // pushing arguments from the stack into variables in the new scope
                    for (Node::Iterator it=x.list()[1].list().begin(); it != x.list()[1].list().end(); ++it)
                    {
                        m_code_pages.back().emplace_back(Instruction::LET);
                        std::size_t var_id = addSymbol(it->getStringVal());
                        pushNumber(static_cast<uint16_t>(var_id), &(m_code_pages.back()));
                    }
                    // push body of the function
                    _compile(x.list()[2], m_code_pages.size() - 1);
                    // return last value on the stack
                    m_code_pages.back().emplace_back(Instruction::RET);
                }
                else if (n == Keyword::Begin)
                {
                    for (std::size_t i=1; i < x.list().size(); ++i)
                        _compile(x.list()[i], p);
                    
                    // return last value
                    page(p).push_back(Instruction::RET);
                }
                else if (n == Keyword::While)
                {
                    // save current position to jump there at the end of the loop
                    std::size_t current = page(p).size();
                    // push condition
                    _compile(x.list()[1], p);
                    // push code to temp page
                        m_temp_pages.emplace_back();
                        _compile(x.list()[2], -static_cast<int>(m_temp_pages.size()));
                    // relative jump to end of block if condition is false
                    page(p).emplace_back(Instruction::POP_JUMP_IF_FALSE);
                    // relative address to jump to if condition is false, casted as unsigned (don't worry, it's normal)
                    pushNumber(static_cast<uint16_t>(m_temp_pages.back().size()), &page(p));
                    // copy code from temp page and destroy temp page
                    for (auto&& inst : m_temp_pages.back())
                        page(p).push_back(inst);
                    m_temp_pages.pop_back();
                    // loop, jump to the condition
                    page(p).emplace_back(Instruction::JUMP);
                    // relative address casted as unsigned (don't worry, it's normal)
                    pushNumber(static_cast<uint16_t>(current), &page(p));
                }

                return;
            }

            // if we are here, we should have a function name
            // push arguments first, then function name, then call it
                m_temp_pages.emplace_back();
                _compile(x.list()[0], -static_cast<int>(m_temp_pages.size()));  // storing proc
            // push arguments on current page
            for (Node::Iterator exp=x.list().begin() + 1; exp != x.list().end(); ++exp)
                _compile(*exp, p);
            // push proc from temp page
            for (auto&& inst : m_temp_pages.back())
                page(p).push_back(inst);
            m_temp_pages.pop_back();
            // call the procedure
            page(p).push_back(Instruction::CALL);
            // number of arguments
            pushNumber(static_cast<uint16_t>(std::distance(x.list().begin() + 1, x.list().end())));

            return;
        }

        std::size_t Compiler::addSymbol(const std::string& sym)
        {
            // check if sym is nil/false/true
            if (sym == "nil")
                return 0;
            else if (sym == "false")
                return 1;
            else if (sym == "true")
                return 2;

            // otherwise, add the symbol, and return its id in the table + 3
            // (+3 to have distinct symbols from the builtin ones)
            auto it = std::find(m_symbols.begin(), m_symbols.end(), sym);
            if (it == m_symbols.end())
            {
                m_symbols.push_back(sym);
                return 3 + (m_symbols.size() - 1);
            }
            return 3 + ((std::size_t) std::distance(m_symbols.begin(), it));
        }

        std::size_t Compiler::addValue(Node x)
        {
            Value v(x);
            auto it = std::find(m_values.begin(), m_values.end(), v);
            if (it == m_values.end())
            {
                m_values.push_back(v);
                return m_values.size() - 1;
            }
            return (std::size_t) std::distance(m_values.begin(), it);
        }

        std::size_t Compiler::addValue(std::size_t page_id)
        {
            Value v(page_id);
            auto it = std::find(m_values.begin(), m_values.end(), v);
            if (it == m_values.end())
            {
                m_values.push_back(v);
                return m_values.size() - 1;
            }
            return (std::size_t) std::distance(m_values.begin(), it);
        }

        void Compiler::pushNumber(uint16_t n, std::vector<Inst>* page)
        {
            if (page == nullptr)
            {
                m_bytecode.push_back((n & 0xff00) >> 8);
                m_bytecode.push_back(n & 0x00ff);
            }
            else
            {
                page->emplace_back((n & 0xff00) >> 8);
                page->emplace_back(n & 0x00ff);
            }
        }
    }
}