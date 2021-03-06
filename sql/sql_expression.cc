/** sql_expression.cc
    Jeremy Barnes, 24 January 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

    This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

    Basic components of SQL expressions.
*/

#include "sql_expression.h"
#include "mldb/base/parse_context.h"
#include "mldb/arch/demangle.h"
#include "mldb/types/structure_description.h"
#include "mldb/types/vector_description.h"
#include "mldb/types/enum_description.h"
#include "mldb/types/pair_description.h"
#include "mldb/types/map_description.h"
#include "mldb/jml/utils/environment.h"
#include "sql_expression_operations.h"
#include "table_expression_operations.h"
#include "interval.h"
#include "tokenize.h"
#include "mldb/core/dataset.h"
#include "mldb/http/http_exception.h"
#include "mldb/server/dataset_context.h"
#include "mldb/types/value_description.h"

#include <mutex>

#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string.hpp>


using namespace std;

namespace Datacratic {
namespace MLDB {

/*****************************************************************************/
/* CONSTANTS                                                                 */
/*****************************************************************************/

const SelectExpression SelectExpression::STAR("*");
const std::shared_ptr<SqlExpression> SqlExpression::TRUE
    = SqlExpression::parse("true");
const std::shared_ptr<SqlExpression> SqlExpression::ONE
    = SqlExpression::parse("1.0");
const WhenExpression WhenExpression::TRUE(SqlExpression::TRUE);
const OrderByExpression OrderByExpression::ROWHASH
    (OrderByExpression::parse("rowHash() ASC"));


/*****************************************************************************/
/* KNOWN COLUMN                                                              */
/*****************************************************************************/

DEFINE_STRUCTURE_DESCRIPTION(KnownColumn);

KnownColumnDescription::
KnownColumnDescription()
{
    addField("columnName", &KnownColumn::columnName,
             "Name of column");
    addField("valueInfo", &KnownColumn::valueInfo,
             "Information about the range of values the column can be");
    addField("sparsity", &KnownColumn::sparsity,
             "Is the column dense or sparse?");
    addField("offset", &KnownColumn::offset,
             "What is the offset of the column in the row (if it's fixed)?",
             (int32_t)-1);
}


/*****************************************************************************/
/* GENERATE ROWS WHERE FUNCTION                                              */
/*****************************************************************************/


DEFINE_STRUCTURE_DESCRIPTION(GenerateRowsWhereFunction);

GenerateRowsWhereFunctionDescription::
GenerateRowsWhereFunctionDescription()
{
    addField("explain", &GenerateRowsWhereFunction::explain,
             "Explanation of the generated values");
    addField("orderedBy", &GenerateRowsWhereFunction::orderedBy,
             "Describe how values are ordered");
}


/*****************************************************************************/
/* BOUND ROW EXPRESSION                                                      */
/*****************************************************************************/

// HACK; remove
static std::shared_ptr<const SqlExpression>
getExpressionFromPtr(const SqlExpression * expr)
{
    if (typeid(*expr) == typeid(SelectExpression))
        return std::make_shared<SelectExpression>(*(SelectExpression *)expr);
    return expr->shared_from_this();
}

BoundSqlExpression::
BoundSqlExpression(ExecFunction exec,
                   const SqlExpression * expr,
                   std::shared_ptr<ExpressionValueInfo> info,
                   BoundExpressionMetadata metadata)
    : exec(std::move(exec)),
      expr(getExpressionFromPtr(expr)),
      info(std::move(info)),
      metadata(std::move(metadata))
{
}

ExpressionValue
BoundSqlExpression::
constantValue() const
{
    return expr->constantValue();

    if (!metadata.isConstant) {
        throw HttpReturnException
            (400, "Attempt to extract constant from non-constant expression.  "
             "One of the elements of the expression requires a constant "
             "value, for example the text of a regular expression, but was "
             "actually a non-constant expression: '" + expr->surface + "'",
             "surface", expr->surface,
             "ast", expr->print());
    }

    // This is only OK to do here because by setting isConstant in its metadata,
    // the expression is guaranteeing it will never access its context.
    SqlRowScope context;
    return operator () (context, GET_LATEST);
}

DEFINE_STRUCTURE_DESCRIPTION(BoundSqlExpression);

BoundSqlExpressionDescription::
BoundSqlExpressionDescription()
{
    addField("info", &BoundSqlExpression::info,
             "Information on the result returned from the expression");
    addField("expr", &BoundSqlExpression::expr,
             "Expression that was bound");
    //addField("ast", &BoundSqlExpression::ast,
    //         "Abstract syntax tree of expression");
    //addField("surface", &BoundSqlExpression::surface,
    //         "Surface form of expression");
}


/*****************************************************************************/
/* ROW EXPRESSION BINDING CONTEXT                                            */
/*****************************************************************************/

SqlBindingScope::
SqlBindingScope() : functionStackDepth(0)
{
}

SqlBindingScope::
~SqlBindingScope()
{
}

namespace {

std::recursive_mutex externalFunctionsMutex;
std::unordered_map<Utf8String, ExternalFunction> externalFunctions;

std::recursive_mutex externalDatasetFunctionsMutex;
std::unordered_map<Utf8String, ExternalDatasetFunction> externalDatasetFunctions;


} // file scope

std::shared_ptr<void> registerFunction(Utf8String name, ExternalFunction function)
{
    auto unregister = [=] (void *)
        {
            //cerr << "unregistering external function " << name << endl;
            std::unique_lock<std::recursive_mutex> guard(externalFunctionsMutex);
            externalFunctions.erase(name);
        };

    std::unique_lock<std::recursive_mutex> guard(externalFunctionsMutex);
    if (!externalFunctions.insert({name, std::move(function)}).second)
        throw HttpReturnException(400, "Attempt to double register function",
                                  "name", name);

    //cerr << "registering external function " << name << endl;
    return std::shared_ptr<void>(nullptr, unregister);
}

ExternalFunction lookupFunction(const Utf8String & name)
{
    auto res = tryLookupFunction(name);
    if (!res)
        throw HttpReturnException(400, "Couldn't find function",
                                  "name", name);
    return res;
}

ExternalFunction tryLookupFunction(const Utf8String & name)
{
    std::unique_lock<std::recursive_mutex> guard(externalFunctionsMutex);
    auto it = externalFunctions.find(name);
    if (it == externalFunctions.end())
        return nullptr;
    return it->second;
}

BoundFunction
SqlBindingScope::
doGetFunction(const Utf8String & tableName,
              const Utf8String & functionName,
              const std::vector<BoundSqlExpression> & args,
              SqlBindingScope & argScope)
{
    auto factory = tryLookupFunction(functionName);
    if (factory) {
        return factory(functionName, args, argScope);
    }

    if (functionName == "leftRowName")
        throw HttpReturnException(400, "Function 'leftRowName' is not available outside of a join");

    if (functionName == "rightRowName")
        throw HttpReturnException(400, "Function 'rightRowName' is not available outside of a join");

    if (functionName == "leftRowPath")
        throw HttpReturnException(400, "Function 'leftRowPath' is not available outside of a join");

    if (functionName == "rightRowPath")
        throw HttpReturnException(400, "Function 'rightRowPath' is not available outside of a join");
    
    return {nullptr, nullptr};
}

//These are functions in table expression, i.e. in FROM clauses

std::shared_ptr<void> registerDatasetFunction(Utf8String name, ExternalDatasetFunction function)
{
    auto unregister = [=] (void *)
        {
            //cerr << "unregistering external function " << name << endl;
            std::unique_lock<std::recursive_mutex> guard(externalDatasetFunctionsMutex);
            externalDatasetFunctions.erase(name);
        };

    std::unique_lock<std::recursive_mutex> guard(externalDatasetFunctionsMutex);
    if (!externalDatasetFunctions.insert({name, std::move(function)}).second)
        throw HttpReturnException(400, "Attempt to double register Dataset function",
                                  "name", name);

    //cerr << "registering external function " << name << endl;
    return std::shared_ptr<void>(nullptr, unregister);
}

ExternalDatasetFunction tryLookupDatasetFunction(const Utf8String & name)
{
    std::unique_lock<std::recursive_mutex> guard(externalDatasetFunctionsMutex);
    auto it = externalDatasetFunctions.find(name);
    if (it == externalDatasetFunctions.end())
        return nullptr;
    return it->second;
}

BoundTableExpression
SqlBindingScope::
doGetDatasetFunction(const Utf8String & functionName,
                     const std::vector<BoundTableExpression> & args,
                     const ExpressionValue & options,
                     const Utf8String & alias)
{
    auto factory = tryLookupDatasetFunction(functionName);
    if (factory) {
        return factory(functionName, args, options, *this, alias);
    }
    
    return BoundTableExpression();
}

namespace {

std::recursive_mutex externalAggregatorsMutex;
std::unordered_map<Utf8String, ExternalAggregator> externalAggregators;

} // file scope

std::shared_ptr<void> registerAggregator(Utf8String name, ExternalAggregator aggregator)
{
    auto unregister = [=] (void *)
        {
            std::unique_lock<std::recursive_mutex> guard(externalAggregatorsMutex);
            externalAggregators.erase(name);
        };

    std::unique_lock<std::recursive_mutex> guard(externalAggregatorsMutex);
    if (!externalAggregators.insert({name, std::move(aggregator)}).second)
        throw HttpReturnException(400, "Attempt to double register aggregator",
                                  "name", name);

    return std::shared_ptr<void>(nullptr, unregister);
}

ExternalAggregator lookupAggregator(const Utf8String & name)
{
    auto res = tryLookupAggregator(name);
    if (!res)
        throw HttpReturnException(400, "Couldn't find aggregator",
                                  "name", name);
    return res;
}

ExternalAggregator tryLookupAggregator(const Utf8String & name)
{
    std::unique_lock<std::recursive_mutex> guard(externalAggregatorsMutex);
    auto it = externalAggregators.find(name);
    if (it == externalAggregators.end())
        return nullptr;
    return it->second;
}

BoundAggregator
SqlBindingScope::
doGetAggregator(const Utf8String & aggregatorName,
                const std::vector<BoundSqlExpression> & args)
{
    auto factory = tryLookupAggregator(aggregatorName);
    if (factory) {
        return factory(aggregatorName, args, *this);
    }
    
    return {nullptr, nullptr};
    //throw HttpReturnException(400, "Binding context " + ML::type_name(*this)
    //                    + " must override getAggregator: wanted "
    //                    + aggregatorName);
}

ColumnGetter
SqlBindingScope::
doGetColumn(const Utf8String & tableName, const ColumnName & columnName)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                              + " must override getColumn: wanted "
                              + columnName.toUtf8String());
}

GetAllColumnsOutput
SqlBindingScope::
doGetAllColumns(const Utf8String & tableName,
                std::function<ColumnName (const ColumnName &)> keep)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                        + " must override getAllColumns: wanted "
                        + tableName);
}

GenerateRowsWhereFunction
SqlBindingScope::
doCreateRowsWhereGenerator(const SqlExpression & where,
                  ssize_t offset,
                  ssize_t limit)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                        + " must override doCreateRowsWhereGenerator");
}

ColumnFunction
SqlBindingScope::
doGetColumnFunction(const Utf8String & functionName)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                        + " must override doGetColumnFunction");
}

ColumnGetter
SqlBindingScope::
doGetBoundParameter(const Utf8String & paramName)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                              + " does not support bound parameters ($1... or $name)");
}

std::shared_ptr<Dataset>
SqlBindingScope::
doGetDataset(const Utf8String & datasetName)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                              + " does not support getting datasets");
}

std::shared_ptr<Dataset>
SqlBindingScope::
doGetDatasetFromConfig(const Any & datasetConfig)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                              + " does not support getting datasets");
}

TableOperations
SqlBindingScope::
doGetTable(const Utf8String & tableName)
{
    throw HttpReturnException(500, "Binding context " + ML::type_name(*this)
                              + " does not support getting tables");
}

ColumnName
SqlBindingScope::
doResolveTableName(const ColumnName & fullColumnName,
                   Utf8String &tableName) const
{
    //default behaviour is there is no dataset so return the full column name
    return fullColumnName;
}

MldbServer *
SqlBindingScope::
getMldbServer() const
{
    return nullptr;
}

/*****************************************************************************/
/* SCOPED NAME                                                               */
/*****************************************************************************/

bool
ScopedName::
operator == (const ScopedName & other) const
{
    return scope == other.scope && name == other.name;
}

bool
ScopedName::
operator != (const ScopedName & other) const
{
    return ! operator == (other);
}

bool
ScopedName::
operator < (const ScopedName & other) const
{
    return (scope < other.scope)
        || ((scope == other.scope)
            && (name < other.name));
}

DEFINE_STRUCTURE_DESCRIPTION(ScopedName);

ScopedNameDescription::
ScopedNameDescription()
{
    addField("scope", &ScopedName::scope,
             "Scope in which this variable is being looked up",
             Utf8String());
    addField("name", &ScopedName::name,
             "Name of the variable, in its scope");
}


/*****************************************************************************/
/* UNBOUND ENTITIES                                                          */
/*****************************************************************************/

DEFINE_STRUCTURE_DESCRIPTION(UnboundVariable);

UnboundVariableDescription::
UnboundVariableDescription()
{
    addField("info", &UnboundVariable::info,
             "Type information known about unbound variable");
}

void
UnboundVariable::
merge(UnboundVariable var)
{
    if (!var.info)
        return;
    if (!info) {
        info = var.info;
        return;
    }

    info = ExpressionValueInfo::getCovering(info, var.info);
}

DEFINE_STRUCTURE_DESCRIPTION(UnboundWildcard);

UnboundWildcardDescription::
UnboundWildcardDescription()
{
    addField("prefix", &UnboundWildcard::prefix,
             "Prefix with which the wildcard will be added");
}

void
UnboundWildcard::
merge(UnboundWildcard wc)
{
    if (wc.prefix.empty())
        return;
    if (prefix.empty()) {
        prefix = wc.prefix;
        return;
    }
}

DEFINE_STRUCTURE_DESCRIPTION(UnboundFunction);

UnboundFunctionDescription::
UnboundFunctionDescription()
{
    addField("argsForArity", &UnboundFunction::argsForArity,
             "Arguments for a given function arity");
}

void
UnboundFunction::
merge(UnboundFunction func)
{
    // TODO: merge the argument descriptions
    for (auto & f: func.argsForArity) {
        argsForArity.insert(std::make_pair(f.first, std::move(f.second)));
    }
}

void
UnboundTable::
merge(UnboundTable table)
{
    for (auto & t: table.vars) {
        vars[t.first].merge(std::move(t.second));
    }

    for (auto & w: table.wildcards) {
        wildcards[w.first].merge(std::move(w.second));
    }

    for (auto & t: table.funcs) {
        funcs[t.first].merge(std::move(t.second));
    }
}

DEFINE_STRUCTURE_DESCRIPTION(UnboundTable);

UnboundTableDescription::
UnboundTableDescription()
{
    addField("vars", &UnboundTable::vars,
             "Variables that need to be available within this table's scope");
    addField("wildcards", &UnboundTable::wildcards,
             "Wildcards that will be matched within this table's scope");
    addField("funcs", &UnboundTable::funcs,
             "Functions that will be matched within this table's scope");
}

void
UnboundEntities::
merge(UnboundEntities other)
{
    for (auto & t: other.tables) {
        tables[t.first].merge(std::move(t.second));
    }
    
    for (auto & v: other.vars) {
        vars[v.first].merge(std::move(v.second));
    }

    for (auto & w: other.wildcards) {
        wildcards[w.first].merge(std::move(w.second));
    }
    
    for (auto & v: other.funcs) {
        funcs[v.first].merge(std::move(v.second));
    }

    for (auto & p: other.params) {
        params[p.first].merge(std::move(p.second));
    }
}

void
UnboundEntities::
mergeFiltered(UnboundEntities other,
              const std::set<Utf8String> & knownTables)
{
    // Remove the known tables
    for (auto & t: knownTables)
        other.tables.erase(t);

    // Now merge what is left
    merge(std::move(other));
}

bool
UnboundEntities::
hasUnboundVariables() const
{
    if (!vars.empty())
        return true;

    if (!wildcards.empty())
        return true;

    for (auto & t: tables) {
        if (!t.second.vars.empty())
            return true;
        if (!t.second.wildcards.empty())
            return true;
    }

    return false;
}

bool
UnboundEntities::
hasRowFunctions() const
{
    // False coupling to improve. See MLDB-1769
    return funcs.find("columnCount") != funcs.end()
        || funcs.find("rowHash") != funcs.end()
        || funcs.find("rowPath") != funcs.end()
        || funcs.find("leftRowHash") != funcs.end()
        || funcs.find("rightRowHash") != funcs.end();
}

DEFINE_STRUCTURE_DESCRIPTION(UnboundEntities);

UnboundEntitiesDescription::
UnboundEntitiesDescription()
{
    addField("tables", &UnboundEntities::tables,
             "Tables that are unbound from the expression");
    addField("vars", &UnboundEntities::vars,
             "Variables (unscoped) that are unbound from the expression");
    addField("wildcards", &UnboundEntities::wildcards,
             "Wildcards (unscoped) that are unbound from the expression");
    addField("funcs", &UnboundEntities::funcs,
             "Functions (unscoped) that are called from the expression");
    addField("params", &UnboundEntities::params,
             "Query parameters that are unbound from the expression");
}


/*****************************************************************************/
/* SQL ROW SCOPE                                                             */
/*****************************************************************************/

// Environment variable that tells us whether we check the row scope types
// or not, which may be more expensive.
ML::Env_Option<bool> MLDB_CHECK_ROW_SCOPE_TYPES
("MLDB_CHECK_ROW_SCOPE_TYPES", false);

// Visible manifestation of that variable.  We statically initialize it here
// so that it can still be accessed before shared library initialization.
bool SqlRowScope::checkRowScopeTypes = false;

namespace {
// Initialize with the final value once the library loads
struct InitializeCheckRowScopeTypes {
    InitializeCheckRowScopeTypes()
    {
        SqlRowScope::checkRowScopeTypes = MLDB_CHECK_ROW_SCOPE_TYPES;
    }
};
} // file scope

void
SqlRowScope::
throwBadNestingError(const std::type_info & typeRequested,
                     const std::type_info & typeFound)
{
    std::string t_req = ML::demangle(typeRequested.name());
    std::string t_found = ML::demangle(typeFound.name());
    throw HttpReturnException(500, "Invalid scope nesting: requested "
                              + t_req + " got " + t_found,
                              "typeRequested", t_req,
                              "typeFound", t_found);
}


/*****************************************************************************/
/* SQL EXPRESSION                                                            */
/*****************************************************************************/

SqlExpression::
~SqlExpression()
{
}

namespace {

// Match a non-scoped identifier
static Utf8String matchIdentifier(ML::Parse_Context & context,
                                  bool allowUtf8)
{
    Utf8String result;

    if (context.eof())
        return result;

    if (context.match_literal('"')) {
        //read until the double quote closes.
        {
            ML::Parse_Context::Revert_Token token(context);

            for (;;) {
                if (context.match_literal("\"\"")) {
                    result += '"';
                }
                else if (context.match_literal('"')) {
                    token.ignore();
                    return result;
                }
                else if (!context) {
                    break;
                }
                else result += expectUtf8Char(context);           
            }
        }
        
        // If we get here, we had EOF inside a string
        context.exception("No closing quote character for string");
    }
    else {

        //un-enclosed

        // An identifier can't start with a digit (MLDB-200)
        if (isdigit(*context))
            return result;

        while (context && (isalnum(*context) || *context == '_'))
            result += *context++;
    }

    return result;
}

static ColumnName matchColumnName(ML::Parse_Context & context, bool allowUtf8)
{
    ColumnName result;

    if (context.eof())
        return result;

    Utf8String first = matchIdentifier(context, allowUtf8);

    if (first.empty())
        return result;
    result = PathElement(std::move(first));

    while (context.match_literal('.')) {
        Utf8String next = matchIdentifier(context, allowUtf8);
        if (next.empty())
            break;  // will happen for a *
        result = result + next;
    }

    return result;
}

bool matchBlockCommentStart(ML::Parse_Context & ctx)
{
    if (ctx.eof() || *ctx != '/')
        return false;

    {
        ML::Parse_Context::Revert_Token token(ctx);
        ++ctx;
        if (!ctx.eof() && (*ctx == '*')) {
            ++ctx;
            token.ignore();
            return true;
        }
    }

    return false;

}

void skipToBlockCommentEnd(ML::Parse_Context & ctx)
{
    while (!ctx.eof()) {
        if (*ctx == '*') {
            ++ctx;
            if (!ctx.eof() && *ctx == '/') {
                ++ctx;
                return;
            }
        }
        else {
            ++ctx;
        }
    }
}

bool matchLineCommentStart(ML::Parse_Context & ctx)
{
    if (ctx.eof() || *ctx != '-')
        return false;

    {
        ML::Parse_Context::Revert_Token token(ctx);
        ++ctx;
        if (!ctx.eof() && (*ctx == '-')) {
            ++ctx;
            token.ignore();
            return true;
        }
    }

    return false;

}

void skipToEndOfLine(ML::Parse_Context & ctx)
{
    while (!ctx.eof()) {
        if (ctx.match_eol())
            return;
        else
            ++ctx;
    }
}

// Parse_Context doesn't consider \n to be a whitespace...
bool match_whitespace(ML::Parse_Context & ctx)
{
    bool result = false;
    bool inBlockComment = false;
    bool inLineComment = false;
    while (!ctx.eof() && (isspace(*ctx) || (inBlockComment = matchBlockCommentStart(ctx)) || (inLineComment = matchLineCommentStart(ctx)))) {
        result = true;

        if (inBlockComment) {
            skipToBlockCommentEnd(ctx);
            inBlockComment = false;
        }
        else if (inLineComment) {
            skipToEndOfLine(ctx);
            inLineComment = false;
        }
        else {
            ctx++;
        }   
    }
    return result;
}

void skip_whitespace(ML::Parse_Context & ctx)
{
    match_whitespace(ctx);
}

void expect_whitespace(ML::Parse_Context & ctx)
{
    if (!match_whitespace(ctx)) ctx.exception("expected whitespace");
}

// Match a keyword in any case
static bool matchKeyword(ML::Parse_Context & context, const char * keyword)
{
    ML::Parse_Context::Revert_Token token(context);

    skip_whitespace(context);
    const char * p = keyword;

    while (context && *p) {
        if (*p == ' ') {
            if (!isspace(*context))
                return false;
            skip_whitespace(context);
            ++p;
            continue;
        }

        if (tolower(*context) != tolower(*p))
            return false;

        ++context;
        ++p;
    }

    if (*p == 0) {
        token.ignore();
        return true;
    }

    return false;
}

// Expect a keyword in any case
static void expectKeyword(ML::Parse_Context & context, const char * keyword)
{
    if (!matchKeyword(context, keyword)) {
        context.exception("expected keyword " + string(keyword));
    }
}

// Read ahead to see if a keyword matches
static bool peekKeyword(ML::Parse_Context & context, const char * keyword)
{
    ML::Parse_Context::Revert_Token token(context);
    return matchKeyword(context, keyword);
}

void matchSingleQuoteStringAscii(ML::Parse_Context & context, std::string& resultStr)
{
    {
        ML::Parse_Context::Revert_Token token(context);

        for (;;) {
            if (context.match_literal("\'\'"))
                resultStr += '\'';
            else if (context.match_literal('\'')) {
                token.ignore();
                return;
            }
            else if (!context)
                break;  // eof inside string
            else if (*context < 0 || *context > 127)
                context.exception("Non-ASCII character in ASCII context");
            else resultStr += *context++;
        }
    }

    // If we get here, we had EOF inside a string
    context.exception("No closing quote character for string");
}

void matchSingleQuoteStringUTF8(ML::Parse_Context & context,
                                std::basic_string<char32_t>& resultStr)
{
    {
        ML::Parse_Context::Revert_Token token(context);

        for (;;) {
            if (context.match_literal("\'\'"))
                resultStr += '\'';
            else if (context.match_literal('\'')) {
                token.ignore();
                return;
            }
            else if (!context) {
                break;  // eof inside string
            }
            else resultStr += expectUtf8Char(context);
        }
    }

    // If we get here, we had EOF inside a string
    context.exception("No closing quote character for string");
}

bool matchConstant(ML::Parse_Context & context, ExpressionValue & result,
                   bool allowUtf8)
{
    double d_num;
    long long ll_num;
    
    if (matchKeyword(context, "true")) {
        result = ExpressionValue(true, Date::negativeInfinity());
        return true;
    }
    else if (matchKeyword(context, "false")) {
        result = ExpressionValue(false, Date::negativeInfinity());
        return true;
    }
    else if (matchKeyword(context, "null")) {
        result = ExpressionValue();
        return true;
    }
    else if (matchKeyword(context, "nan")) {
        result = ExpressionValue(std::numeric_limits<double>::quiet_NaN(),
                                 Date::negativeInfinity());
        return true;
    }
    else if (matchKeyword(context, "infinity") || matchKeyword(context, "inf")) {
        result = ExpressionValue(INFINITY,
                                 Date::negativeInfinity());
        return true;
    }
    else if (matchKeyword(context, "interval"))
    {
        uint32_t months = 0, days = 0;
        double seconds = 0.0f;

        skip_whitespace(context);
        char closingLiteral = '\"';

        if (context.match_literal('\''))
        {
            closingLiteral = '\'';
        }
        else
        {
            context.expect_literal(closingLiteral);
        }

        expect_interval(context, months, days, seconds);

        context.expect_literal(closingLiteral);

        result = ExpressionValue::fromInterval(months, days, seconds,
                                 Date::negativeInfinity());
        return true;
    }
    // be strict when matching float otherwise integer will be rounded
    else if (context.match_double(d_num, -INFINITY, INFINITY, false)) {
        result = ExpressionValue(d_num, Date::negativeInfinity());
        return true;
    }
    else if (context.match_long_long(ll_num)) {
        result = ExpressionValue(ll_num, Date::negativeInfinity());
        return true;
    }
    else if (context.match_literal('\'')) {
        if (!allowUtf8) {
            std::string resultStr;
            matchSingleQuoteStringAscii(context, resultStr);
            result = ExpressionValue(resultStr, Date::negativeInfinity());
            return true;
        }
        else {
            std::basic_string<char32_t> resultStr;
            matchSingleQuoteStringUTF8(context, resultStr);
            Utf8String utf8String(resultStr);
            result = ExpressionValue(resultStr, Date::negativeInfinity());
            return true;
        }
    }

    else return false;
}


// Match an operator in any case
static bool matchOperator(ML::Parse_Context & context, const char * keyword)
{
    ML::Parse_Context::Revert_Token token(context);

    const char * p = keyword;

    while (context && *p) {
        if (tolower(*context) != tolower(*p))
            return false;

        ++context;
        ++p;
    }

    if (*p == 0) {
        // Alphabetic chars must be followed by appropriate trailing context, ie
        // something that's not an alphanumeric character
        if (isalpha(*keyword)) {
            if (context && (isalnum(*context) || *context == '_'))
                return false;
        }
        
        token.ignore();
        return true;
    }

    return false;
}

bool
matchJoinQualification(ML::Parse_Context & context, JoinQualification& joinQualify)
{
    joinQualify = JOIN_INNER;
    bool inner = matchKeyword(context, "INNER ");
    if (!inner)
    {
        bool right = false;
        bool full = false;
        bool outer = false;
        bool left = matchKeyword(context, "LEFT ");
        if (!left)
        {
            right = matchKeyword(context, "RIGHT ");
            if (!right)
            {
               full = matchKeyword(context, "FULL ");
               outer = matchKeyword(context, "OUTER ");
            }
        }

        if (right || left || full || outer)
        {
           //outer is optional, eat it
           context.skip_whitespace();
           if (!outer)
              matchKeyword(context, "OUTER ");

           joinQualify = right ? JOIN_RIGHT : (left ? JOIN_LEFT : JOIN_FULL);

           //MUST match the 'JOIN'
           expectKeyword(context, "JOIN ");
           return true;
        }
        else
        {
           return matchKeyword(context, "JOIN ");
        }
    }
    else
    {
        expectKeyword(context,"JOIN ");
        return true;
    }

    return false;
}

const SqlExpression::Operator operators[] = {
    //symbol, unary, handler, precedence, description
    { "~", true,         SqlExpression::bwise,  1, "Bitwise NOT" },
    { "timestamp", true, SqlExpression::func,   1, "Coercion to timestamp" },
    { "@", false,        SqlExpression::func,   2, "Set timestamp" },
    { "*", false,        SqlExpression::arith,  2, "Multiplication" },
    { "/", false,        SqlExpression::arith,  2, "Division" },
    { "%", false,        SqlExpression::arith,  2, "Modulo" },
    { "+", true,         SqlExpression::arith,  3, "Unary positive" },
    { "-", true,         SqlExpression::arith,  3, "Unary negative" },
    { "+", false,        SqlExpression::arith,  3, "Addition / Concatenation" },
    { "-", false,        SqlExpression::arith,  3, "Subtraction" },
    { "&", false,        SqlExpression::bwise,  3, "Bitwise and" },
    { "|", false,        SqlExpression::bwise,  3, "Bitwise or" },
    { "^", false,        SqlExpression::bwise,  3, "Bitwise exclusive or" },
    { "=", false,        SqlExpression::compar, 4, "Equality" },
    { ">=", false,       SqlExpression::compar, 4, "Greater or equal to" },
    { "<=", false,       SqlExpression::compar, 4, "Less or equal to" },
    { "<>", false,       SqlExpression::compar, 4, "" },
    { "!=", false,       SqlExpression::compar, 4, "Not equal to" },
    { "!>", false,       SqlExpression::compar, 4, "Not greater than" },
    { "!<", false,       SqlExpression::compar, 4, "Not less than" },
    { ">", false,        SqlExpression::compar, 4, "Greater than" },
    { "<", false,        SqlExpression::compar, 4, "Less than" },
    { "NOT", true,       SqlExpression::booln,  5, "Unary not" },
    { "AND", false,      SqlExpression::booln,  6, "Boolean and" },
    { "OR", false,       SqlExpression::booln,  7, "Boolean or" },
    { "ALL", true,       SqlExpression::unimp,  7, "All true" },
    { "ANY", true,       SqlExpression::unimp,  7, "Any true" },
    { "SOME", true,      SqlExpression::unimp,  7, "Some true" }
};

} // file scope

std::shared_ptr<SqlExpression>
SqlExpression::
parse(ML::Parse_Context & context, int currentPrecedence, bool allowUtf8)
{
    skip_whitespace(context);

    ML::Parse_Context::Hold_Token token(context);

    //cerr << "parsing at context " << context.get_offset() << " precedence " << currentPrecedence
    //     << " char " << *context << endl;

    // If not, start an expression
    std::shared_ptr<SqlExpression> lhs;

    if (context.match_literal('(')) {
        // Precedence resets since we're inside parentheses
        auto expr = parse(context, 10 /* precedence */, allowUtf8);
        skip_whitespace(context);
        context.expect_literal(')');
        expr->surface = token.captured();
        lhs = expr;
    }

    // Look for a bound parameter
    if (!lhs && context.match_literal('$')) {
        int paramIndex;
        if (context.match_int(paramIndex)) {
            lhs.reset(new BoundParameterExpression(Utf8String(std::to_string(paramIndex))));
            lhs->surface = ML::trim(token.captured());
        }
        else {
            Utf8String paramName = matchIdentifier(context, allowUtf8);
            if (paramName.empty())
                context.exception("Expected identifier after $");
            lhs.reset(new BoundParameterExpression(paramName));
            lhs->surface = ML::trim(token.captured());
        }
    }

    // Otherwise, look for a unary operator.  That becomes the lhs.
    if (!lhs) {
        for (const Operator & op: operators) {
            if (!op.unary)
                continue;
            if (op.precedence > currentPrecedence) {
                /* Will need to be bound outside our expression, since the precedence is wrong. */
                break;
            }
            if (matchOperator(context, op.token)) {
                auto rhs = parse(context, op.precedence, allowUtf8);
                auto expr = op.handler(nullptr /* no lhs */, rhs, op.token);
                expr->surface = token.captured();
                lhs = expr;
                break;
            }
        }
    }    
    if (!lhs && context.match_literal('{')) {
        // We get a select clause
        skip_whitespace(context);

        vector<std::shared_ptr<SqlRowExpression> > clauses;

        if (!context.match_literal('}')) {
            do {
                skip_whitespace(context);
                auto expr = SqlRowExpression::parse(context, allowUtf8);
                skip_whitespace(context);
                clauses.emplace_back(std::move(expr));
            } while (context.match_literal(','));

            skip_whitespace(context);
            context.expect_literal('}');
        }

        if (clauses.size() != 1) {
            auto select = std::make_shared<SelectExpression>(clauses);
            auto arg = std::make_shared<SelectWithinExpression>(select);
            lhs = arg;
        }
        else {
            ExcAssertEqual(1, clauses.size());
            auto arg = std::make_shared<SelectWithinExpression>(clauses[0]);
            lhs = arg;  
        }

        lhs->surface = ML::trim(token.captured());
    }

    if (!lhs && context.match_literal('[')) {
        // We get an embedding clause
        skip_whitespace(context);

        vector<std::shared_ptr<SqlExpression> > clauses;
        if (!context.match_literal(']')) {
            do {
                context.skip_whitespace();
                auto expr = SqlExpression::parse(context, 10, allowUtf8);
                context.skip_whitespace();
                clauses.emplace_back(std::move(expr));
            } while (context.match_literal(','));

            skip_whitespace(context);
            context.expect_literal(']');
        }

        lhs = std::make_shared<EmbeddingLiteralExpression>(clauses);
        
        lhs->surface = ML::trim(token.captured());
    }

    if (!lhs && matchKeyword(context, "CAST")) {
        skip_whitespace(context);
        context.expect_literal('(');

        std::shared_ptr<SqlExpression> expr
            = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);
        
        skip_whitespace(context);
        expectKeyword(context, "AS ");
        
        std::string type = matchIdentifier(context, allowUtf8).extractAscii();
        if (type.empty())
            context.exception("Expected type name as identifier");
        boost::to_lower(type);

        skip_whitespace(context);
        context.expect_literal(')');

        lhs = std::make_shared<CastExpression>(expr, type);
        lhs->surface = ML::trim(token.captured());
    }

    if (!lhs && matchKeyword(context, "CASE")) {
        expect_whitespace(context);

        std::shared_ptr<SqlExpression> expr;
        std::vector<std::pair<std::shared_ptr<SqlExpression>,
                              std::shared_ptr<SqlExpression> > > when;
        std::shared_ptr<SqlExpression> elseExpr;

        if (!peekKeyword(context, "WHEN")) {
            // Simple case expression; we have an expression
            expr = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);
        }
        
        while (matchKeyword(context, "WHEN")) {
            expect_whitespace(context);
            auto key = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);
            expectKeyword(context, "THEN");
            expect_whitespace(context);
            auto val = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);

            when.emplace_back(std::move(key), std::move(val));
            skip_whitespace(context);
        }
        
        if (matchKeyword(context, "ELSE")) {
            expect_whitespace(context);
            elseExpr = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);
        }

        expectKeyword(context, "END");
        
        lhs = std::make_shared<CaseExpression>(std::move(expr), std::move(when),
                                               std::move(elseExpr));
        lhs->surface = ML::trim(token.captured());
    }

    // Otherwise, look for a constant
    if (!lhs) {
        ExpressionValue constant;
        if (matchConstant(context, constant, allowUtf8)) {
            lhs = std::make_shared<ConstantExpression>(constant);
            lhs->surface = ML::trim(token.captured());
        }
    }

    // First, look for a variable or a function name
    if (!lhs) {
        ColumnName identifier = matchColumnName(context, allowUtf8);
        if (!identifier.empty()) {
            lhs = std::make_shared<ReadColumnExpression>(identifier);
            lhs->surface = ML::trim(token.captured());

            skip_whitespace(context);
            if (context.match_literal('(')) {

                // Function call.  Get the arguments
                skip_whitespace(context);

                // count(*) and vertical_count(*) are special and need special
                // parsing support (look in the SQLite syntax diagram... there
                // is a special syntax node
                bool checkGlob
                    = identifier.toUtf8String() == "count"
                    || identifier.toUtf8String() == "vertical_count";
                
                std::vector<std::shared_ptr<SqlExpression> > args;                    
                while (!context.match_literal(')')) {
                    skip_whitespace(context);
                    
                    if (checkGlob && args.size() == 0
                        && context.match_literal('*')) {
                        //count is *special*
                        auto arg = make_shared<ConstantExpression>
                            (ExpressionValue(true, Date::negativeInfinity()));
                        args.emplace_back(std::move(arg));
                        skip_whitespace(context);
                        context.match_literal(')');
                        break;
                    }

                    auto arg = parse(context,
                                     10 /* precedence reset for comma */,
                                     allowUtf8);
                    args.emplace_back(std::move(arg));
                    skip_whitespace(context);
                    
                    if (context.match_literal(')'))
                        break;

                    skip_whitespace(context);
                    context.expect_literal(',');
                }

                // For a function call, the last element is always the name
                // of the function.  The rest is the path through the table.

                Utf8String tableName, functionName;
                if (identifier.size() == 1) {
                    functionName = identifier[0].toUtf8String();
                }
                else if (identifier.size() == 2) {
                    tableName = identifier[0].toUtf8String();
                    functionName = identifier[1].toUtf8String();
                }
                else {
                    context.exception
                        ("Ambiguous function name.  There should be one or two "
                         "elements; either tableName.functionName or "
                         "functionName.  If your table name contains dots, put "
                         "them in quotes.  For example, to access the rowName() "
                         "function in table x.y, use \"x.y\".rowName() instead "
                         "of x.y.rowName()");
                }

                lhs = std::make_shared<FunctionCallExpression>
                    (tableName, functionName, args);

                lhs->surface = ML::trim(token.captured());

                skip_whitespace(context);

                // Look for an extract expression, which allows us to rewrite
                // the output of the function.
                std::shared_ptr<SqlExpression> extractExpression;
                if (context.match_literal('[')) {
                    //extract brackets for user functions
                    auto extractExpression
                        = SqlExpression::parse(context,
                                               10 /*precedence*/, allowUtf8);
                    skip_whitespace(context);
                    context.expect_literal(']');

                    lhs = std::make_shared<ExtractExpression>(lhs, extractExpression);
                    lhs->surface = ML::trim(token.captured());
                }
            } // if '(''
        } // if ! identifier empty
    } //if (!lhs)


    if (lhs) {
        if (lhs->surface == "")
            cerr << lhs->print() << endl;
        ExcAssertNotEqual(lhs->surface, "");
    }

    // Otherwise, there is nothing to start the expression with
    if (!lhs)
        context.exception("Expected identifier or constant");

    while (true) {
        skip_whitespace(context);
        
        if (context.eof()) {
            lhs->surface = ML::trim(token.captured());
            return lhs;
        }

        // Look for IS NULL or IS NOT NULL
        if (matchKeyword(context, "IS")) {
            expect_whitespace(context);
            bool notExpr = false;
            if (matchKeyword(context, "NOT")) {
                notExpr = true;
                expect_whitespace(context);
            }

            if (matchKeyword(context, "NULL"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "null");
            else if (matchKeyword(context, "TRUE"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "true");
            else if (matchKeyword(context, "FALSE"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "false");
            else if (matchKeyword(context, "STRING"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "string");
            else if (matchKeyword(context, "NUMBER"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "number");
            else if (matchKeyword(context, "INTEGER"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "integer");
            else if (matchKeyword(context, "TIMESTAMP"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "timestamp");
            else if (matchKeyword(context, "INTERVAL"))
                lhs = std::make_shared<IsTypeExpression>(lhs, notExpr, "interval");
            else context.exception("Expected NULL, TRUE, FALSE, STRING, NUMBER, INTEGER, TIMESTAMP or INTERVAL after IS {NOT}");

            lhs->surface = ML::trim(token.captured());

            continue;
        }

        // Between expression
        bool notBetween = false;
        // BETWEEN precedence is the same as > operator
        if (currentPrecedence > 4) {
            if (matchKeyword(context, "BETWEEN")
                || ((notBetween = true) && matchKeyword(context, "NOT BETWEEN"))) {
                expect_whitespace(context);
                auto lower = SqlExpression::parse(context, 5 /* precedence */, allowUtf8);
                expectKeyword(context, "AND");
                auto upper = SqlExpression::parse(context, 5 /* precedence */, allowUtf8);
            
                lhs = std::make_shared<BetweenExpression>(lhs, lower, upper, notBetween);
                lhs->surface = ML::trim(token.captured());
                continue;
            }
        }

        // 'In' expression

        bool negative = false;
        // IN and NOT IN precedence is the same as the NOT operator
        if (currentPrecedence > 5) {
            if ((negative = matchKeyword(context, "NOT IN")) ||
                (!peekKeyword(context, "INNER") && matchKeyword(context, "IN"))) {
                expect_whitespace(context);

                context.expect_literal('(');
                skip_whitespace(context);
                if (peekKeyword(context, "SELECT")) {
                    //sub-table
                    auto statement = SelectStatement::parse(context, allowUtf8);
                           
                    skip_whitespace(context);
                    context.expect_literal(')');

                    Utf8String asName("");

                    auto rhs = std::make_shared<SelectSubtableExpression>(statement, asName);
                    lhs = std::make_shared<InExpression>(lhs, rhs, negative);
                    lhs->surface = ML::trim(token.captured());
                }
                else if (matchKeyword(context, "KEYS OF")) {
                    auto rhs = SqlExpression::parse(context, 10, allowUtf8);
                    skip_whitespace(context);
                    context.expect_literal(')');
                    lhs = std::make_shared<InExpression>(lhs, rhs, negative, InExpression::KEYS);
                    lhs->surface = ML::trim(token.captured());                
                }
                else if (matchKeyword(context, "VALUES OF")) {
                    auto rhs = SqlExpression::parse(context, 10, allowUtf8);
                    skip_whitespace(context);
                    context.expect_literal(')');
                    lhs = std::make_shared<InExpression>(lhs, rhs, negative, InExpression::VALUES);
                    lhs->surface = ML::trim(token.captured());                
                }
                else {
                    auto rhs = std::make_shared<TupleExpression>(TupleExpression::parse(context, allowUtf8));

                    context.expect_literal(')');
                
                    lhs = std::make_shared<InExpression>(lhs, rhs, negative);
                    lhs->surface = ML::trim(token.captured());
                    continue;
                }
            }
        }

        // 'LIKE' expression
        if (currentPrecedence > 5) {
            if ((negative = matchKeyword(context, "NOT LIKE")) || matchKeyword(context, "LIKE")) {
                expect_whitespace(context);

                auto rhs = SqlExpression::parse(context, 5, allowUtf8);

                lhs = std::make_shared<LikeExpression>(lhs, rhs, negative);
                lhs->surface = ML::trim(token.captured());
            }
        }

        // Now look for an operator
        bool found = false;
        for (const Operator & op: operators) {
            if (op.unary)
                continue;
            if (op.precedence >= currentPrecedence) {
                /* Will need to be bound outside our expression, since the precence is wrong. */
                break;
            }
            if (matchOperator(context, op.token)) {
                auto rhs = parse(context, op.precedence, allowUtf8);
                lhs = op.handler(lhs, rhs, op.token);
                lhs->surface = ML::trim(token.captured());
                found = true;
                break;
            }
        }
        
        if (!found) {
            lhs->surface = ML::trim(token.captured());
            return lhs;
        }
    }
}

std::shared_ptr<SqlExpression>
SqlExpression::
parse(const std::string & expression, const std::string & filename,
      int row, int col)
{
    ML::Parse_Context context(filename.empty() ? expression : filename,
                              expression.c_str(),
                              expression.length(), row, col);
    auto result = parse(context, 10 /* starting precedence */, false /* allowUtf8 */);
    skip_whitespace(context);
    context.expect_eof();
    //cerr << "result of " << expression << " is " << result->print() << endl;
    return result;
}

std::shared_ptr<SqlExpression>
SqlExpression::
parse(const char * expression, const std::string & filename,
      int row, int col)
{
    return parse(string(expression), filename, row, col);
}

std::shared_ptr<SqlExpression>
SqlExpression::
parse(const Utf8String & expression, const std::string & filename,
      int row, int col)
{
    ML::Parse_Context context(filename.empty() ? expression.rawData() : filename,
                              expression.rawData(),
                              expression.rawLength(), row, col);
    auto result = parse(context, 10 /* starting precedence */, true /* allowUtf8 */);
    skip_whitespace(context);
    context.expect_eof();
    //cerr << "result of " << expression << " is " << result->print() << endl;
    return result;
}

std::shared_ptr<SqlExpression>
SqlExpression::
parseDefault(ExpressionValue def,
             const std::string & expression,
             const std::string & filename,
             int row, int col)
{
    if (expression.empty())
        return std::make_shared<ConstantExpression>(def);
    else return parse(expression, filename, row, col);
}

std::shared_ptr<SqlExpression>
SqlExpression::
substitute(const SelectExpression & toSubstitute) const
{
    throw HttpReturnException(400, "SqlExpression::substitute");
#if 0
    // Pattern to match is getVariable(name).  We replace with the
    // substituted version from the expression.

    MatchVariable<std::string> columnName("columnName");

    auto result
        = applyTransform(MatchGetVariable(columnName),
                         [&] (const MatchContext & match,
                              std::shared_ptr<consSqlExpression> expr)
                         -> std::shared_ptr<SqlExpression>
                         {
                             string var = match[columnName];
                             auto substitution = toSubstitute.tryGetNamedVariable(var);
                             if (!substitution)
                                 return expr;
                             return substitution;
                         });

    return result;
#endif
}

bool
SqlExpression::
isConstant() const
{
    for (auto & c: getChildren()) {
        if (!c->isConstant())
            return false;
    }
    return true;
}

ExpressionValue
SqlExpression::
constantValue() const
{
    SqlExpressionConstantScope scope;
    auto bound = this->bind(scope);
    SqlRowScope rowScope = scope.getRowScope();
    return bound(rowScope, GET_LATEST);
}

std::map<ScopedName, UnboundVariable>
SqlExpression::
variableNames() const
{
    std::map<ScopedName, UnboundVariable> result;
    
    for (auto & c: getChildren()) {
        auto childVars = (*c).variableNames();
        for (auto & cv: childVars) {
            result[cv.first].merge(std::move(cv.second));
        }
    }
    
    return result;
}

std::map<ScopedName, UnboundWildcard>
SqlExpression::
wildcards() const
{
    std::map<ScopedName, UnboundWildcard> result;
    
    for (auto & c: getChildren()) {
        auto childWildcards = (*c).wildcards();
        for (auto & cw: childWildcards) {
            result[cw.first].merge(std::move(cw.second));
        }
    }
    
    return result;
}

std::map<ScopedName, UnboundFunction>
SqlExpression::
functionNames() const
{
    std::map<ScopedName, UnboundFunction> result;
    
    for (auto & c: getChildren()) {
        auto childFuncs = (*c).functionNames();
        for (auto & cv: childFuncs) {
            result[cv.first].merge(std::move(cv.second));
        }
    }
    
    return result;
}

std::map<Utf8String, UnboundVariable>
SqlExpression::
parameterNames() const
{
    std::map<Utf8String, UnboundVariable> result;
    
    for (auto & c: getChildren()) {
        auto childVars = (*c).parameterNames();
        for (auto & cv: childVars) {
            result[cv.first].merge(std::move(cv.second));
        }
    }
    
    return result;
}

UnboundEntities
SqlExpression::
getUnbound() const
{
    UnboundEntities result;

    for (auto & p: parameterNames()) {
        result.params[p.first].merge(p.second);
    }

    for (auto & v: variableNames()) {
        if (v.first.scope.empty())
            result.vars[v.first.name].merge(v.second);
        else
            result.tables[v.first.scope].vars[v.first.name].merge(v.second);
    }

    for (auto & w: wildcards()) {
        if (w.first.scope.empty())
            result.wildcards[w.first.name].merge(w.second);
        else
            result.tables[w.first.scope].wildcards[w.first.name].merge(w.second);
    }

    for (auto & v: functionNames()) {
        // toSimpleName() is OK, since functions can't have compound names
        if (v.first.scope.empty())
            result.funcs[v.first.name.toSimpleName()].merge(v.second);
        else
            result.tables[v.first.scope].funcs[v.first.name].merge(v.second);
    }
    
    for (auto & c: getChildren()) {
        result.merge(c->getUnbound());
    }
    
    return result;
}

std::shared_ptr<SqlExpression>
SqlExpression::
shallowCopy() const
{
    auto onArgs = [] (std::vector<std::shared_ptr<SqlExpression> > args)
        -> std::vector<std::shared_ptr<SqlExpression> >
        {
            return std::move(args);
        };

    return transform(onArgs);
}

void
SqlExpression::
traverse(const TraverseFunction & visitor) const
{
    auto type = getType();
    auto operation = getOperation();
    auto children = getChildren();
    
    if (!visitor(*this, type, operation, children))
        return;

    for (auto & c: children) {
        c->traverse(visitor);
    }
}

bool
SqlExpression::
isIdentitySelect(SqlExpressionDatasetScope & context) const
{
    return false;  // safe default; subclasses can override for better perf
}

bool
SqlExpression::
isConstantTrue() const
{
    return isConstant() && constantValue().isTrue();
}

bool
SqlExpression::
isConstantFalse() const
{
    return isConstant() && constantValue().isFalse();
}

std::shared_ptr<SqlExpression>
SqlExpression::
bwise(std::shared_ptr<SqlExpression> lhs,
      std::shared_ptr<SqlExpression> rhs,
      const std::string & op)
{
    return std::make_shared<BitwiseExpression>(lhs, rhs, op);
}

std::shared_ptr<SqlExpression>
SqlExpression::
arith(std::shared_ptr<SqlExpression> lhs,
      std::shared_ptr<SqlExpression> rhs,
      const std::string & op)
{
    return std::make_shared<ArithmeticExpression>(lhs, rhs, op);
}

std::shared_ptr<SqlExpression>
SqlExpression::
compar(std::shared_ptr<SqlExpression> lhs,
       std::shared_ptr<SqlExpression> rhs,
       const std::string & op)
{
    return std::make_shared<ComparisonExpression>(lhs, rhs, op);
}

std::shared_ptr<SqlExpression>
SqlExpression::
booln(std::shared_ptr<SqlExpression> lhs,
      std::shared_ptr<SqlExpression> rhs,
      const std::string & op)
{
    return std::make_shared<BooleanOperatorExpression>(lhs, rhs, op);
}

std::shared_ptr<SqlExpression>
SqlExpression::
func(std::shared_ptr<SqlExpression> lhs,
     std::shared_ptr<SqlExpression> rhs,
     const std::string & op)
{
    static std::map<std::string, Utf8String> funcMap = {{"@", "at"}, {"timestamp", "to_timestamp"}};
    std::vector<std::shared_ptr<SqlExpression> > args;
    if (lhs)
        args.push_back(lhs); // binary operator
    args.push_back(rhs);
    return std::make_shared<FunctionCallExpression>("" /* tableName */, funcMap[op], args);
}

std::shared_ptr<SqlExpression>
SqlExpression::
unimp(std::shared_ptr<SqlExpression> lhs,
      std::shared_ptr<SqlExpression> rhs,
      const std::string & op)
{
    throw HttpReturnException(400, "unimplemented operator " + op);
}

//Find aggregators for any class implementing getChildren.
template <class T>
std::vector<std::shared_ptr<SqlExpression> >
findAggregatorsT(const T* expression, bool withGroupBy)
{
    typedef std::vector<std::shared_ptr<SqlExpression> >::iterator IterType;
    std::vector<std::shared_ptr<SqlExpression> > output;
    std::vector<std::shared_ptr<SqlExpression> > children = expression->getChildren();
    
     // collect aggregators
    for (auto iter = children.begin(); iter != children.end(); ++iter)
    {
        auto child = *iter;
        if (child->isAggregator())
            output.push_back(child);
        else {
            //we dont look for aggregators in aggregator - its not legal - this check above
            //order MUST be preserved
            std::vector<std::shared_ptr<SqlExpression> > subchildren = child->getChildren();
            int pos = iter - children.begin();
            children.insert(iter+1, subchildren.begin(), subchildren.end());
            iter = children.begin() + pos;
        }
    }

    std::vector<Utf8String> culprit;

    /**
       For a SELECT expression to be valid in presence of a GROUP BY or an aggregator,
       wildcard expressions must be included in an aggregator directly or indirectly
       - "SELECT earliest(temporal_earliest({*})) GROUP BY something" is valid
       - "SELECT temporal_earliest({*}) GROUP BY something" is not valid
       Similarly for implied GROUP BY (here because of aggregator sum)
       - "SELECT sum({*}), earliest(temporal_earliest({*}))"  is valid but
       - "SELECT sum({*}), temporal_earliest({*})" is not valid
       Other invalid SELECT expressions in presence of GROUP BY like referring to a variable
       not in the GROUP BY expression are detected later when binding these expressions.
       We use the same logic for order by expressions.
    */
    std::function<bool(IterType, IterType)> wildcardNestedInAggregator = [&](IterType begin, IterType end) {
        for (IterType it = begin; it < end; ++it) {
            if (!(*it)->isAggregator()) {
                auto children = (*it)->getChildren();
                if (children.size() == 0  && (*it)->isWildcard()) {
                    return false;
                }
                culprit.push_back((*it)->surface);
                bool result = wildcardNestedInAggregator(children.begin(), children.end());
                culprit.pop_back();
                if (!result) return result;
            }
        }
        return true;
    };
    
    bool wildcardInAggs = wildcardNestedInAggregator(children.begin(), children.end());
 
    if (!wildcardInAggs && (output.size() != 0 || withGroupBy)) {
        throw HttpReturnException(400, (withGroupBy ?
                                        "Non-aggregator '" + culprit.front() + 
                                        "' with GROUP BY clause is not allowed" :
                                        "Mixing non-aggregator '" + culprit.front() + 
                                        "' with aggregators is not allowed"));
    }
    
    return std::move(output);
}

std::vector<std::shared_ptr<SqlExpression> >
SqlExpression::
findAggregators(bool withGroupBy) const
{
    return findAggregatorsT<SqlExpression>(this, withGroupBy);
}

struct SqlExpressionDescription
    : public ValueDescriptionT<std::shared_ptr<SqlExpression> > {

    SqlExpressionDescription();

    virtual void parseJsonTyped(std::shared_ptr<SqlExpression>  * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const std::shared_ptr<SqlExpression>  * val,
                                JsonPrintingContext & context) const;
};

struct ConstSqlExpressionDescription
    : public ValueDescriptionT<std::shared_ptr<const SqlExpression> > {

    ConstSqlExpressionDescription();

    virtual void parseJsonTyped(std::shared_ptr<const SqlExpression>  * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const std::shared_ptr<const SqlExpression>  * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(std::shared_ptr<SqlExpression>,
                            SqlExpressionDescription);
DEFINE_VALUE_DESCRIPTION_NS(std::shared_ptr<const SqlExpression>,
                            ConstSqlExpressionDescription);

SqlExpressionDescription::
SqlExpressionDescription()
{
    setTypeName("SqlValueExpression");
    documentationUri = "/doc/builtin/sql/ValueExpression.md";
}

void
SqlExpressionDescription::
parseJsonTyped(std::shared_ptr<SqlExpression>  * val,
               JsonParsingContext & context) const
{
    *val = SqlExpression::parse(context.expectStringUtf8());
}

void
SqlExpressionDescription::
printJsonTyped(const std::shared_ptr<SqlExpression>  * val,
               JsonPrintingContext & context) const
{
    if (!*val)
        context.writeNull();
    else context.writeStringUtf8((*val)->surface);
}

ConstSqlExpressionDescription::
ConstSqlExpressionDescription()
{
    setTypeName("ConstSqlValueExpression");
}

void
ConstSqlExpressionDescription::
parseJsonTyped(std::shared_ptr<const SqlExpression>  * val,
               JsonParsingContext & context) const
{
    throw HttpReturnException(400, "SqlExpressionDescription::parseJsonTyped");
}

void
ConstSqlExpressionDescription::
printJsonTyped(const std::shared_ptr<const SqlExpression>  * val,
               JsonPrintingContext & context) const
{
    if (!*val)
        context.writeNull();
    else context.writeStringUtf8((*val)->surface);
}


/*****************************************************************************/
/* SQL ROW EXPRESSION                                                        */
/*****************************************************************************/

SqlRowExpression::
~SqlRowExpression()
{
}

std::shared_ptr<SqlRowExpression>
SqlRowExpression::
parse(ML::Parse_Context & context, bool allowUtf8)
{
    ML::Parse_Context::Hold_Token capture(context);

    if (matchKeyword(context, "COLUMN EXPR (")
        || matchKeyword(context, "COLUMN EXPR(")) {
        
        // Components
        // - select: value to select as column; row expression
        // - as: how to name the resulting column
        // - where: expression to limit what matches
        // - order by: list of columns to select
        // - limit, offset: restrict number of columns

        std::shared_ptr<SqlExpression> select;
        std::shared_ptr<SqlExpression> as;
        std::shared_ptr<SqlExpression> when;
        std::shared_ptr<SqlExpression> where;
        OrderByExpression orderBy;
        int64_t offset = 0;
        int64_t limit = -1;

        if (matchKeyword(context, "SELECT ")) {
            select = SqlExpression::parse(context, 10, allowUtf8);
            // Select eats whitespace
        }
        else select = SqlExpression::parse("value()");

        if (matchKeyword(context, "AS ")) {
            as = SqlExpression::parse(context, 10, allowUtf8);
            // As eats whitespace
        }
        else as = SqlExpression::parse("columnPath()");
        
        if (matchKeyword(context, "WHEN ")) {
            throw HttpReturnException(400, "WHEN clause not supported in row expression");
        }
        else when = SqlExpression::parse("true");

        if (matchKeyword(context, "WHERE ")) {
            where = SqlExpression::parse(context, 10, allowUtf8);
            // Where expression consumes whitespace
        }
        else where = SqlExpression::parse("true");

        if (matchKeyword(context, "ORDER BY ")) {
            orderBy = OrderByExpression::parse(context, allowUtf8);
            // Order by expression consumes whitespace
        }

        if (matchKeyword(context, "OFFSET ")) {
            offset = context.expect_long_long(0);
            if (context && *context != ')')
                expect_whitespace(context);
        }

        if (matchKeyword(context, "LIMIT ")) {
            limit = context.expect_long_long(0);
            if (context && *context != ')')
                expect_whitespace(context);
        }
        
        context.expect_literal(')');

        auto result = std::make_shared<SelectColumnExpression>(select, as, where, orderBy,
                                                               offset, limit);
        result->surface = capture.captured();
        return result;
    }

    /* Match either:

       <prefix>*

       OR

       *

       as a filtered subset or all columns from the output.
    */
    auto matchPrefixedWildcard = [&] (ColumnName & prefix)
        {
            ML::Parse_Context::Revert_Token token(context);
            skip_whitespace(context);
            prefix = matchColumnName(context, allowUtf8);
            if (context.match_literal('*')) {
                token.ignore();
                return true;
            }
            return false;
        };

    skip_whitespace(context);

    bool isWildcard = false;
    bool matched = false;
    ColumnName prefix;
    ColumnName prefixAs;
    std::vector<std::pair<ColumnName, bool> > exclusions;   // Prefixes to exclude
    std::shared_ptr<SqlExpression> expr;
    ColumnName columnName;

    {
        ML::Parse_Context::Revert_Token token(context);

        if (matchPrefixedWildcard(prefix)) {
            // Sort out ambiguity between * operator and wildcard by looking at trailing
            // context.
            //
            // It can only be a wildcard if followed by:
            // - eof
            // - a comma
            // - closing parenthesis, if used as an expression
            // - AS
            // - EXCLUDING
            // - a keyword: FROM, WHERE, GROUP BY, HAVING, LIMIT, OFFSET

            ML::Parse_Context::Revert_Token token2(context);

            skip_whitespace(context);
            if (context.eof() || context.match_literal(',') || context.match_literal(')') || context.match_literal('}')
                || matchKeyword(context, "AS") || matchKeyword(context, "EXCLUDING")
                || matchKeyword(context, "NAMED")
                || matchKeyword(context, "FROM") || matchKeyword(context, "WHERE")
                || matchKeyword(context, "GROUP BY") || matchKeyword(context, "HAVING")
                || matchKeyword(context, "LIMIT") || matchKeyword(context, "OFFSET")) {
                isWildcard = true;
                matched = true;
                token.ignore();
            }
        }
    }

    // MLDB-1002 case 1: x: y <--> y AS x
    if (!matched) {
        // Allow backtracking if we don't find a colon
        ML::Parse_Context::Revert_Token token(context);

        // Do we have an identifier?
        ColumnName asName = matchColumnName(context, allowUtf8);
        
        if (!asName.empty()) {

            skip_whitespace(context);

            // Followed by a colon?
            if (context.match_literal(':')) {
                token.ignore();
                columnName = asName;
                skip_whitespace(context);
                expr = SqlExpression::parse(context, 10, allowUtf8);
                auto result = std::make_shared<NamedColumnExpression>
                    (columnName, expr);
                result->surface = capture.captured();
                return result;
            }
        }
    }

    // MLDB-1002 case 2: x*: y* <--> y* AS x*
    if (!matched) {
        // Allow backtracking if we don't find a colon
        ML::Parse_Context::Revert_Token token(context);

        // Do we have an identifier?
        if (matchPrefixedWildcard(prefix)) {

            skip_whitespace(context);

            // Followed by a colon?
            if (context.match_literal(':')) {

                if (matchPrefixedWildcard(prefixAs)) {
                    token.ignore();

                    auto result = std::make_shared<WildcardExpression>
                        (prefix, prefixAs, exclusions);
                    result->surface = ML::trim(capture.captured());
                    return result;
                }
            }
        }
    }

    //cerr << "matched is now " << matched << endl;

    if (matched) {
        ExcAssert(isWildcard);

        // There may be an excluding expression
        // Syntax:
        // EXCLUDING (<prefix>*)
        // EXCLUDING (<name>)
        // EXCLUDING (<prefix1>*, <prefix2>*, <name1>, ...)

        skip_whitespace(context);

        if (matchKeyword(context, "EXCLUDING")) {

            auto expectExclusion = [&] ()
                {
                    ColumnName prefix = matchColumnName(context, allowUtf8);
                    if (context.match_literal('*')) {
                        if (prefix.empty())
                            context.exception("can't exclude *");
                        exclusions.emplace_back(prefix, true);
                    }
                    else {
                        if (prefix.empty())
                            context.exception("Expected column name or prefixed wildcard for exclusion");
                        exclusions.emplace_back(prefix, false);
                    }
                };

            auto expectExclusionList = [&] ()
                {
                    skip_whitespace(context);
                    if (!context.match_literal(')')) {
                        expectExclusion();
                        skip_whitespace(context);
                        while (context.match_literal(',')) {
                            skip_whitespace(context);
                            expectExclusion();
                            skip_whitespace(context);
                        }
                        context.expect_literal(')');
                    }
                };

            match_whitespace(context);
            context.expect_literal('(');
            expectExclusionList();
        }

        skip_whitespace(context);

        // There may be an AS clause
        // AS <prefix>*
        // that will modify the matched prefix
        // examples:
        //
        // svd* AS mysvd*
        // * AS my*
        
        if (matchKeyword(context, "AS")) {
            skip_whitespace(context);
            if (!matchPrefixedWildcard(prefixAs))
                context.exception("Expected prefixed wildcard for AS wildcard expression");
        }
        else prefixAs = prefix;

        auto result = std::make_shared<WildcardExpression>
            (prefix, prefixAs, exclusions);
        result->surface = ML::trim(capture.captured());
        return result;
    }

    // It's an expression
    expr = SqlExpression::parse(context, 10, allowUtf8);
    matched = true;

    skip_whitespace(context);

    if (matchKeyword(context, "AS")) {
        skip_whitespace(context);
        if (context.match_literal('*')) {
            // No alias for rows
        }
        else {
            columnName = matchColumnName(context, allowUtf8);
            if (columnName.empty())
                context.exception("Expected identifier as name of variable");
        }
    } else {
        auto colExpr = std::dynamic_pointer_cast<ReadColumnExpression>(expr);
        if (colExpr)
            columnName = colExpr->columnName;
        else columnName = PathElement(expr->surface);
    }

    auto result = std::make_shared<NamedColumnExpression>(columnName, expr);

    result->surface = capture.captured();
    return result;
}


std::shared_ptr<SqlRowExpression>
SqlRowExpression::
parse(const std::string & expression, const std::string & filename,
      int row, int col)
{
    ML::Parse_Context context(filename.empty() ? expression : filename,
                              expression.c_str(),
                              expression.length(), row, col);
    auto result = parse(context, false /* allowUtf8 */);
    skip_whitespace(context);
    context.expect_eof();
    return result;
}

std::shared_ptr<SqlRowExpression>
SqlRowExpression::
parse(const char *  expression, const std::string & filename,
      int row, int col)
{
    return parse(string(expression), filename, row, col);
}

std::vector<std::shared_ptr<SqlRowExpression> >
SqlRowExpression::
parseList(ML::Parse_Context & context, bool allowUtf8)
{
    std::vector<std::shared_ptr<SqlRowExpression> > result;

    for (;;) {
        skip_whitespace(context);
        if (context.eof())
            break;

        auto expr = SqlRowExpression::parse(context, allowUtf8);
        if (!expr)
            break;
        result.push_back(expr);

        skip_whitespace(context);
        
        if (context.match_literal(','))
            continue;
        break;
    }
    
    return result;
}

std::vector<std::shared_ptr<SqlRowExpression> >
SqlRowExpression::
parseList(const std::string & expression,
          const std::string & filename, int row, int col)
{
    //cerr << "parsing " << expression << endl;
    ML::Parse_Context context(filename.empty() ? expression : filename,
                              expression.c_str(),
                              expression.length(), row, col);
    auto result = parseList(context, false);
    skip_whitespace(context);
    context.expect_eof();
    return result;
}

std::vector<std::shared_ptr<SqlRowExpression> >
SqlRowExpression::
parseList(const char * expression,
          const std::string & filename, int row, int col)
{
    return parseList(string(expression), filename, row, col);
}

std::vector<std::shared_ptr<SqlRowExpression> >
SqlRowExpression::
parseList(const Utf8String & expression,
          const std::string & filename, int row, int col)
{
    //cerr << "parsing " << expression << endl;
    ML::Parse_Context context(filename.empty() ? expression.rawData() : filename,
                              expression.rawData(),
                              expression.rawLength(), row, col);
    auto result = parseList(context, true);
    skip_whitespace(context);
    context.expect_eof();
    return result;
}

#if 0
std::vector<std::shared_ptr<SqlRowExpression> >
SqlRowExpression::
parseList(const std::vector<std::string> & exprs,
          const std::string & filename, int row, int col)
{
    std::vector<std::shared_ptr<SqlRowExpression> > result;

    for (auto & expr: exprs) {
        result.push_back(parse(expr, filename, false /* allowUtf8 */));
    }

    return result;
}
#endif

struct SqlRowExpressionDescription
    : public ValueDescriptionT<std::shared_ptr<SqlRowExpression> > {

    virtual void parseJsonTyped(std::shared_ptr<SqlRowExpression>  * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const std::shared_ptr<SqlRowExpression>  * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(std::shared_ptr<SqlRowExpression>,
                            SqlRowExpressionDescription);

void
SqlRowExpressionDescription::
parseJsonTyped(std::shared_ptr<SqlRowExpression> * val,
               JsonParsingContext & context) const
{
    throw HttpReturnException(400, "parseJsonTyped for SqlRowExpressionDescription");
}

void
SqlRowExpressionDescription::
printJsonTyped(const std::shared_ptr<SqlRowExpression> * val,
               JsonPrintingContext & context) const
{
    if (!*val)
        context.writeNull();
    else context.writeStringUtf8((*val)->surface);
}


/*****************************************************************************/
/* BOUND ORDER BY EXPRESSION                                                 */
/*****************************************************************************/

DEFINE_STRUCTURE_DESCRIPTION(BoundOrderByClause);

BoundOrderByClauseDescription::
BoundOrderByClauseDescription()
{
    addField("expr", &BoundOrderByClause::expr,
             "Bound expression to calculate the value of the field");
    addField("dir", &BoundOrderByClause::dir,
             "Direction of the sorting");
}

DEFINE_STRUCTURE_DESCRIPTION(BoundOrderByExpression);

BoundOrderByExpressionDescription::
BoundOrderByExpressionDescription()
{
    addField("clauses", &BoundOrderByExpression::clauses,
             "Clauses of the bound expression.  Each one corresponds to an "
             "order by expression.");
}

std::vector<ExpressionValue>
BoundOrderByExpression::
apply(const SqlRowScope & context) const
{
    std::vector<ExpressionValue> sortFields(clauses.size());
    for (unsigned i = 0;  i < clauses.size();  ++i) {
        sortFields[i] = std::move(clauses[i].expr(context, GET_LATEST));
    }
    return sortFields;
}

int
BoundOrderByExpression::
compare(const std::vector<ExpressionValue> & vec1,
        const std::vector<ExpressionValue> & vec2,
        int offset) const
{
    {
        ExcAssertGreaterEqual(vec1.size(), offset + clauses.size());
        ExcAssertGreaterEqual(vec2.size(), offset + clauses.size());

        for (unsigned i = 0;  i < clauses.size();  ++i) {
            const ExpressionValue & e1 = vec1[offset + i];
            const ExpressionValue & e2 = vec2[offset + i];
            int cmp = e1.compare(e2);
            //ExcAssertEqual(e1.compare(e1), 0);
            //ExcAssertEqual(e2.compare(e2), 0);
            //ExcAssertEqual(e2.compare(e1), -cmp);
            if (clauses[i].dir == DESC)
                cmp *= -1;
            if (cmp != 0)
                return cmp;
        }
        
        return 0;
    };
}


/*****************************************************************************/
/* ORDER BY EXPRESSION                                                       */
/*****************************************************************************/

OrderByExpression::
OrderByExpression()
{
}

OrderByExpression::
OrderByExpression(std::vector<std::pair<std::shared_ptr<SqlExpression>, OrderByDirection> > clauses)
    : clauses(std::move(clauses))
{
}

OrderByExpression::
OrderByExpression(TupleExpression clauses)
{
    for (auto & c: clauses.clauses) {
        this->clauses.emplace_back(std::move(c), ASC);
    }
}

OrderByExpression
OrderByExpression::
parse(const std::string & str)
{
    ML::Parse_Context context(str, str.c_str(), str.length());
    OrderByExpression result = parse(context, false /* allowUtf8 */);
    context.expect_eof("Unexpected characters at end of order by expression");
    return result;
}

OrderByExpression
OrderByExpression::
parse(const char * str)
{
    return parse(string(str));
}

OrderByExpression
OrderByExpression::
parse(const Utf8String & str)
{
    ML::Parse_Context context(str.rawData(), str.rawData(), str.rawLength());
    OrderByExpression result = parse(context, true /* allowUtf8 */);
    context.expect_eof("Unexpected characters at end of order by expression");
    return result;
}

OrderByExpression
OrderByExpression::
parse(ML::Parse_Context & context, bool allowUtf8)
{
    ML::Parse_Context::Hold_Token token(context);

    OrderByExpression result;

    skip_whitespace(context);

    while (context) {
        auto expr = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);
        if (!expr)
            break;
        skip_whitespace(context);

        OrderByDirection dir = ASC;
        if (matchKeyword(context, "ASC")) {
        }
        else if (matchKeyword(context, "DESC")) {
            dir = DESC;
        }

        result.clauses.emplace_back(expr, dir);

        skip_whitespace(context);
        
        if (!context.match_literal(','))
            break;
    }
    
    skip_whitespace(context);

    result.surface = token.captured();
    
    return result;
}

Utf8String
OrderByExpression::
print() const
{
    Utf8String result("");

    for (auto & c: clauses) {
        if (!result.empty())
            result += ", ";
        result += c.first->surface;
        if (c.second == DESC) {
            result += " DESC";
        }
    }
    return result;
}

BoundOrderByExpression
OrderByExpression::
bindAll(SqlBindingScope & context) const
{
    BoundOrderByExpression result;
    for (auto & c: clauses) {
        result.clauses.emplace_back(BoundOrderByClause{c.first->bind(context), c.second});
    }
    return result;
}

bool
OrderByExpression::
operator == (const OrderByExpression & other) const
{
    if (other.clauses.size() != clauses.size())
        return false;

    for (unsigned i = 0;  i < clauses.size();  ++i) {
        if (clauses[i].second != other.clauses[i].second)
            return false;
        if (clauses[i].first->surface != other.clauses[i].first->surface)
            return false;
    }

    return true;
}

UnboundEntities
OrderByExpression::
getUnbound() const
{
    UnboundEntities result;
    for (const auto & c: clauses) {
        result.merge(c.first->getUnbound());
    }

    return result;
}

std::vector<std::shared_ptr<SqlExpression> >
OrderByExpression::
findAggregators(bool withGroupBy) const
{
    return findAggregatorsT<OrderByExpression>(this, withGroupBy);
}

std::vector<std::shared_ptr<SqlExpression> >
OrderByExpression::
getChildren() const
{
    std::vector<std::shared_ptr<SqlExpression> > result;

    for (auto & c: clauses) {
        result.push_back(c.first);
    }

    return result;
}

DEFINE_ENUM_DESCRIPTION(OrderByDirection);

OrderByDirectionDescription::
OrderByDirectionDescription()
{
    addValue("ASC", ASC, "Ascending order");
    addValue("DESC", DESC, "Descending order");
}

namespace {
static const auto desc = getDefaultDescriptionSharedT<std::vector<std::pair<std::shared_ptr<SqlExpression>, OrderByDirection> > >();
} // file scpoe

struct OrderByExpressionDescription
    : public ValueDescriptionT<OrderByExpression> {

    OrderByExpressionDescription();

    virtual void parseJsonTyped(OrderByExpression  * val,
                                JsonParsingContext & context) const;
    
    virtual void printJsonTyped(const OrderByExpression * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(OrderByExpression,
                            OrderByExpressionDescription);

OrderByExpressionDescription::
OrderByExpressionDescription()
{
    this->setTypeName("SqlOrderByExpression");
    documentationUri = "/doc/builtin/sql/OrderByExpression.md";
}

void
OrderByExpressionDescription::
parseJsonTyped(OrderByExpression * val,
               JsonParsingContext & context) const
{
    if (context.isString()) {
        *val = OrderByExpression::parse(context.expectStringUtf8());
    }
    else {
        std::vector<std::pair<std::shared_ptr<SqlExpression>, OrderByDirection> > p;
        desc->parseJsonTyped(&p, context);
        val->clauses.swap(p);
    }
}

void
OrderByExpressionDescription::
printJsonTyped(const OrderByExpression * val,
               JsonPrintingContext & context) const
{
    context.writeStringUtf8(val->print());
    //desc->printJsonTyped(&val->clauses, context);
}

OrderByExpression
OrderByExpression::
transform(const TransformArgs & transformArgs) const
{
    OrderByExpression result(*this);

    for (auto & clause: result.clauses)
        clause.first = transformArgs({clause.first})[0];
  
    return std::move(result);
}
    
OrderByExpression
OrderByExpression::
substitute(const SelectExpression & select) const
{
    OrderByExpression result(*this);

    for (auto & clause: result.clauses)
        clause.first = clause.first->substitute(select);
    
    return std::move(result);
}

const OrderByExpression ORDER_BY_NOTHING;


/*****************************************************************************/
/* TUPLE EXPRESSION                                                          */
/*****************************************************************************/

TupleExpression
TupleExpression::
parse(ML::Parse_Context & context, bool allowUtf8)
{
    ML::Parse_Context::Hold_Token token(context);

    TupleExpression result;

    skip_whitespace(context);

    while (context) {
        auto expr = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);

        if (!expr)
            break;
        skip_whitespace(context);

        result.clauses.emplace_back(std::move(expr));

        skip_whitespace(context);
        
        if (!context.match_literal(','))
            break;
    }
    
    skip_whitespace(context);

    result.surface = token.captured();

    return result;
}

TupleExpression
TupleExpression::
parse(const std::string & str)
{
    ML::Parse_Context context(str, str.c_str(), str.length());
    TupleExpression result = parse(context, false /* allowUtf8 */);
    context.expect_eof("Unexpected characters at end of tuple expression");
    return result;
}

TupleExpression
TupleExpression::
parse(const char * str)
{
    return parse(string(str));
}

TupleExpression
TupleExpression::
parse(const Utf8String & str)
{
    ML::Parse_Context context(str.rawData(), str.rawData(), str.rawLength());
    TupleExpression result = parse(context, true /* allowUtf8 */);
    context.expect_eof("Unexpected characters at end of tuple expression");
    return result;
}

Utf8String
TupleExpression::
print() const
{
    Utf8String result("tuple(");

    for (unsigned i = 0;  i < clauses.size();  ++i) {
        if (i != 0)
            result += ", ";
        result += clauses[i]->print();
    }
    
    result += ")";
    return result;
}

TupleExpression 
TupleExpression::
transform(const TransformArgs & transformArgs) const
{
    TupleExpression transformedExpression;

    transformedExpression.clauses = transformArgs(clauses);

    return transformedExpression;
}

bool
TupleExpression::
isConstant() const
{
    bool constant = true;
    for (auto& c : clauses) {
        if (!c->isConstant()) {
            constant = false;
            break;
        }
    }
    return constant;
}

UnboundEntities
TupleExpression::
getUnbound() const
{
    UnboundEntities result;
    for (const auto & c: clauses) {
        result.merge(c->getUnbound());
    }

    return result;
}

struct TupleExpressionDescription
    : public ValueDescriptionT<TupleExpression> {

    TupleExpressionDescription();

    virtual void parseJsonTyped(TupleExpression  * val,
                                JsonParsingContext & context) const;
    
    virtual void printJsonTyped(const TupleExpression * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(TupleExpression,
                            TupleExpressionDescription);

TupleExpressionDescription::
TupleExpressionDescription()
{
    setTypeName("SqlGroupByExpression");
    documentationUri = "/doc/builtin/sql/GroupByExpression.md";
}

void
TupleExpressionDescription::
parseJsonTyped(TupleExpression * val,
               JsonParsingContext & context) const
{
    static const auto desc = getDefaultDescriptionSharedT<std::vector<std::shared_ptr<SqlExpression> > >();

    if (context.isString()) {
        *val = TupleExpression::parse(context.expectStringUtf8());
    }
    else {
        std::vector<std::shared_ptr<SqlExpression> > p;
        desc->parseJsonTyped(&p, context);
        val->clauses.swap(p);
    }
}

void
TupleExpressionDescription::
printJsonTyped(const TupleExpression * val,
               JsonPrintingContext & context) const
{
    if (val->clauses.empty()) {
        context.startArray(0);
        context.endArray();
    }
    else context.writeStringUtf8(val->surface);
}



/*****************************************************************************/
/* SELECT EXPRESSION                                                         */
/*****************************************************************************/

SelectExpression::
SelectExpression()
{
}

SelectExpression::
SelectExpression(const std::string & exprToParse,
                 const std::string & filename,
                 int row, int col)
{
    *this = std::move(parse(exprToParse, filename, row, col));
    ExcAssertEqual(this->surface, exprToParse);
}

SelectExpression::
SelectExpression(const char * exprToParse,
                 const std::string & filename,
                 int row, int col)
{
    *this = std::move(parse(exprToParse, filename, row, col));
    ExcAssertEqual(this->surface, exprToParse);
}

SelectExpression::
SelectExpression(const Utf8String & exprToParse,
                 const std::string & filename,
                 int row, int col)
{
    *this = std::move(parse(exprToParse, filename, row, col));
    ExcAssertEqual(this->surface, exprToParse);
}

SelectExpression::
SelectExpression(std::vector<std::shared_ptr<SqlRowExpression> > clauses)
    : clauses(std::move(clauses))
{
    // concatenate all the surfaces with spaces
    surface = std::accumulate(this->clauses.begin(), this->clauses.end(), Utf8String{},
                              [](const Utf8String & prefix,
                                 std::shared_ptr<SqlRowExpression> & next) {
                                  return prefix.empty() ? next->surface : prefix + ", " + next->surface;
                              });;
}

SelectExpression
SelectExpression::
parse(ML::Parse_Context & context, bool allowUtf8)
{
    SelectExpression result
        = SqlRowExpression::parseList(context, allowUtf8);
    // concatenate all the surfaces with spaces
    result.surface = std::accumulate(result.clauses.begin(), result.clauses.end(), Utf8String{},
                                     [](const Utf8String & prefix,
                                        std::shared_ptr<SqlRowExpression> & next) {
                                         return prefix.empty() ? next->surface : prefix + ", " + next->surface;
                                     });;
    return result;
}

SelectExpression
SelectExpression::
parse(const std::string & expr,
      const std::string & filename, int row, int col)
{
    SelectExpression result
        = SqlRowExpression::parseList(expr, filename, row, col);
    result.surface = expr;
    return result;
}

SelectExpression
SelectExpression::
parse(const char * expr,
      const std::string & filename, int row, int col)
{
    return parse(string(expr), filename, row, col);
}

SelectExpression
SelectExpression::
parse(const Utf8String & expr,
      const std::string & filename, int row, int col)
{
    SelectExpression result
        = SqlRowExpression::parseList(expr, filename, row, col);
    result.surface = expr;
    return result;
}

BoundSqlExpression
SelectExpression::
bind(SqlBindingScope & context) const
{
    vector<BoundSqlExpression> boundClauses;
    for (auto & c: clauses)
        boundClauses.emplace_back(std::move(c->bind(context)));

    std::vector<KnownColumn> outputColumns;

    bool hasUnknownColumns = false;
    bool isConstant = true;
    for (auto & c: boundClauses) {
        ExcAssert(c.info);
        if (c.info->getSchemaCompleteness() == SCHEMA_OPEN) {
            hasUnknownColumns = true;
        }

        auto knownColumns = c.info->getKnownColumns();
        
        outputColumns.insert(outputColumns.end(),
                             knownColumns.begin(),
                             knownColumns.end());

        if (!c.metadata.isConstant)
            isConstant = false;
    }
    
    auto outputInfo = std::make_shared<RowValueInfo>
        (std::move(outputColumns),
         hasUnknownColumns ? SCHEMA_OPEN : SCHEMA_CLOSED);

    auto exec = [=] (const SqlRowScope & context,
                     ExpressionValue & storage,
                     const VariableFilter & filter) -> const ExpressionValue &
        {
            StructValue result;
            result.reserve(boundClauses.size());
            for (auto & c: boundClauses) {
                ExpressionValue storage;
                const ExpressionValue & v = c(context, storage, filter);
                if (&v == &storage)
                    storage.mergeToRowDestructive(result);
                else v.appendToRow(Path(), result);
            }
            
            return storage = std::move(ExpressionValue(std::move(result)));
        };

    return BoundSqlExpression(exec, this, outputInfo, isConstant);
}

Utf8String
SelectExpression::
print() const
{
    Utf8String result("[");
    for (unsigned i = 0;  i < clauses.size();  ++i) {
        if (i > 0)
            result += ", ";
        result += clauses[i]->print();
    }

    result += "]";
    
    return result;
}

std::shared_ptr<SqlExpression>
SelectExpression::
transform(const TransformArgs & transformArgs) const
{
    throw HttpReturnException(400, "Not implemented: SelectExpression::transform()");
}

std::string
SelectExpression::
getType() const
{
    return "select";
}

Utf8String
SelectExpression::
getOperation() const
{
    return Utf8String("row");
}

bool
SelectExpression::
operator == (const SelectExpression & other) const
{
    return surface == other.surface;
}

std::vector<std::shared_ptr<SqlExpression> >
SelectExpression::
getChildren() const
{
    std::vector<std::shared_ptr<SqlExpression> > result;

    for (auto & c: clauses) {
        auto ch = c->getChildren();
        result.insert(result.end(), ch.begin(), ch.end());
    }

    return result;
}

bool
SelectExpression::
isIdentitySelect(SqlExpressionDatasetScope & context) const
{
    // Allow us to identify a select * which will apply the identity
    // function to the row coming in.  This can be used to optimize
    // execution of some expressions.
    return clauses.size() == 1
        && clauses[0]->isIdentitySelect(context);
}

bool
SelectExpression::
isConstant() const
{
    for (auto & c: clauses) {
        if (!c->isConstant())
            return false;
    }
    return true;
}

struct SelectExpressionDescription
    : public ValueDescriptionT<SelectExpression > {

    SelectExpressionDescription();

    virtual void parseJsonTyped(SelectExpression  * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const SelectExpression  * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(SelectExpression, SelectExpressionDescription);

SelectExpressionDescription::
SelectExpressionDescription()
{
    this->setTypeName("SqlSelectExpression");
    documentationUri = "/doc/builtin/sql/SelectExpression.md";
}

void
SelectExpressionDescription::
parseJsonTyped(SelectExpression * val,
               JsonParsingContext & context) const
{
    Utf8String s = context.expectStringUtf8();
    *val = std::move(SelectExpression::parse(s));
}

void
SelectExpressionDescription::
printJsonTyped(const SelectExpression * val,
               JsonPrintingContext & context) const
{
    if (val->clauses.empty())
        context.writeNull();
    else context.writeStringUtf8(val->surface);
}

/*****************************************************************************/
/* JOIN QUALIFICATION                                                        */
/*****************************************************************************/

DEFINE_ENUM_DESCRIPTION(JoinQualification);

JoinQualificationDescription::
JoinQualificationDescription()
{
    addValue("JOIN_INNER", JOIN_INNER, "Inner join");
    addValue("JOIN_LEFT", JOIN_LEFT, "Left join");
    addValue("JOIN_RIGHT", JOIN_RIGHT, "Right join");
    addValue("JOIN_FULL", JOIN_FULL, "Full join");
}

/*****************************************************************************/
/* TABLE EXPRESSION                                                          */
/*****************************************************************************/

TableExpression::
~TableExpression()
{
}

std::shared_ptr<TableExpression>
TableExpression::
parse(ML::Parse_Context & context, int currentPrecedence, bool allowUtf8)
{
    skip_whitespace(context);

    ML::Parse_Context::Hold_Token token(context);

    std::shared_ptr<TableExpression> result;

    auto expectCloseParenthesis = [&] ()
        {
            if (!context.match_literal(')')) {
                context.exception("Expected to find a ')' parsing a table "
                                  "expression.  This is normally because of "
                                  "not putting a sub-SELECT within '()' "
                                  "characters; eg transpose(select 1,2) should "
                                  "be transpose((select 1,2)) so that multiple "
                                  "arguments aren't ambiguous.");
            }
        };

    if (context.match_literal('(')) {

        skip_whitespace(context);
        if (peekKeyword(context, "SELECT"))
        {
            //sub-table
            auto statement = SelectStatement::parse(context, allowUtf8);
            skip_whitespace(context);

            expectCloseParenthesis();

            skip_whitespace(context);
            Utf8String asName;

            if (matchKeyword(context, "AS"))
            {
                expect_whitespace(context);
                asName = matchIdentifier(context, allowUtf8);
                if (asName.empty())
                    context.exception("Expected identifier after the subtable AS clause");
            }

            result.reset(new SelectSubtableExpression(statement, asName));
            result->surface = ML::trim(token.captured());
        }
        else
        {
            result = TableExpression::parse(context, currentPrecedence, allowUtf8);
            skip_whitespace(context);
            expectCloseParenthesis();
            result->surface = ML::trim(token.captured());
        }
    }
    
    // MLDB-1315.  Note that although this looks like a dataset function,
    // in actual fact it's argument is a normal row-valued expression.
    if (matchKeyword(context, "row_dataset")) {
        skip_whitespace(context);
        context.expect_literal('(');
        skip_whitespace(context);
        // Row expression, presented as a table
        auto statement = SqlExpression::parse(context, allowUtf8, 10 /* precedence */);
        skip_whitespace(context);
        expectCloseParenthesis();
        skip_whitespace(context);

        Utf8String asName;
        if (matchKeyword(context, "AS")) {
            expect_whitespace(context);
            asName = matchIdentifier(context, allowUtf8);
            if (asName.empty())
                context.exception("Expected identifier after the row_dataset(...) AS clause");
        }
        
        result = std::make_shared<RowTableExpression>(statement, asName);
        result->surface = ML::trim(token.captured());
    }

    if (!result) {
        std::shared_ptr<NamedDatasetExpression> expr;
        Utf8String identifier = matchIdentifier(context, allowUtf8);

        if (!identifier.empty()) {

            if (context.match_literal('('))
            {
                skip_whitespace(context);
                std::vector<std::shared_ptr<TableExpression>> args;
                std::shared_ptr<SqlExpression> options;
                if (!context.match_literal(')'))
                {
                    do
                    {
                        if(options) {
                            context.exception("options to table expression should "
                                              "be last argument");
                        }

                        skip_whitespace(context);

                        bool found = false;
                        {
                            ML::Parse_Context::Revert_Token token(context);
                            found = context.match_literal('{');
                        }

                        if(found) {
                            options = SqlExpression::parse(context, 10, true);
                        }
                        else {

                            auto subTable = TableExpression::parse(
                                    context, currentPrecedence, allowUtf8);

                            if (subTable)
                                args.push_back(subTable);

                            skip_whitespace(context);
                        }
                    } while (context.match_literal(','));
                }

                expectCloseParenthesis();

                expr.reset(new DatasetFunctionExpression(identifier, args, options));
            }
            else
            {
                expr.reset(new DatasetExpression(identifier, identifier));
            }

            Utf8String asName;

            if (matchKeyword(context, "AS ")) {
                asName = matchIdentifier(context, allowUtf8);
                if (asName.empty())
                    context.exception("Expected identifier after the AS clause");

                expr->setDatasetAlias(asName);
            }

            result = expr;
            result->surface = ML::trim(token.captured());
        }
    }

    if (!result)
        throw HttpReturnException(400, "Expected table expression");

    JoinQualification joinQualify = JOIN_INNER;
    
    while (matchJoinQualification(context, joinQualify)) {
        auto joinTable = TableExpression::parse(context, currentPrecedence, allowUtf8);
            
        std::shared_ptr<SqlExpression> condition;
            
        if (matchKeyword(context, "ON ")) {
            condition = SqlExpression::parse(context, 10 /* precedence */, allowUtf8);
        }
          
        result.reset(new JoinExpression(result, joinTable, condition, joinQualify));
        result->surface = ML::trim(token.captured());
            
        skip_whitespace(context);
    }

    result->surface = ML::trim(token.captured());
    
    return result;
}

std::shared_ptr<TableExpression>
TableExpression::
parse(const Utf8String & expression, const std::string & filename,
      int row, int col)
{
    ML::Parse_Context context(filename.empty() ? expression.rawData() : filename,
                              expression.rawData(),
                              expression.rawLength(), row, col);
    auto result = parse(context, 10 /* starting precedence */, true /* allowUtf8 */);
    skip_whitespace(context);
    context.expect_eof();
    //cerr << "result of " << expression << " is " << result->print() << endl;
    return result;
}

Utf8String
TableExpression::
getAs() const
{
    return Utf8String();
}

void
TableExpression::
printJson(JsonPrintingContext & context)
{
    if (surface.empty())
        throw HttpReturnException(400, "Attempt to write table expression with no surface and no printJson method",
                                  "expressionType", ML::type_name(*this),
                                  "expressionTree", print());
    else context.writeStringUtf8(surface);
}

struct TableExpressionDescription
    : public ValueDescriptionT<std::shared_ptr<TableExpression> > {

    TableExpressionDescription();
    virtual void parseJsonTyped(std::shared_ptr<TableExpression>  * val,
                                JsonParsingContext & context) const;
    
    virtual void printJsonTyped(const std::shared_ptr<TableExpression>  * val,
                                JsonPrintingContext & context) const;
};

struct InputDatasetDescription : public TableExpressionDescription
{
    InputDatasetDescription()
    {
        setTypeName("InputDatasetSpec");
        documentationUri = "/doc/builtin/procedures/InputDatasetSpec.md";
    }
};

std::shared_ptr<ValueDescriptionT<std::shared_ptr<TableExpression> > >
makeInputDatasetDescription()
{
    return std::make_shared<InputDatasetDescription>();
}


struct ConstTableExpressionDescription
    : public ValueDescriptionT<std::shared_ptr<const TableExpression> > {

    ConstTableExpressionDescription();

    virtual void parseJsonTyped(std::shared_ptr<const TableExpression>  * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const std::shared_ptr<const TableExpression>  * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(std::shared_ptr<TableExpression>,
                            TableExpressionDescription);
DEFINE_VALUE_DESCRIPTION_NS(std::shared_ptr<const TableExpression>,
                            ConstTableExpressionDescription);

TableExpressionDescription::
TableExpressionDescription()
{
    setTypeName("SqlFromExpression");
    documentationUri = "/doc/builtin/sql/FromExpression.md";
}

void
TableExpressionDescription::
parseJsonTyped(std::shared_ptr<TableExpression>  * val,
               JsonParsingContext & context) const
{
    if (context.isString())
        *val = TableExpression::parse(context.expectStringUtf8());
    else if (context.isObject()) {
        Json::Value v = context.expectJson();
        val->reset(new DatasetExpression(v, Utf8String()));
        (*val)->surface = v.toStringNoNewLine();
    }
}

void
TableExpressionDescription::
printJsonTyped(const std::shared_ptr<TableExpression>  * val,
               JsonPrintingContext & context) const
{
    if (!*val)
        context.writeNull();
    else (*val)->printJson(context);
}

ConstTableExpressionDescription::
ConstTableExpressionDescription()
{
    setTypeName("ConstSqlFromExpression");
    documentationUri = "/doc/builtin/sql/FromExpression.md";
}

void
ConstTableExpressionDescription::
parseJsonTyped(std::shared_ptr<const TableExpression>  * val,
               JsonParsingContext & context) const
{
    throw HttpReturnException(400, "ConstTableExpressionDescription::parseJsonTyped");
}

void
ConstTableExpressionDescription::
printJsonTyped(const std::shared_ptr<const TableExpression>  * val,
               JsonPrintingContext & context) const
{
    if (!*val)
        context.writeNull();
    else context.writeStringUtf8((*val)->surface);
}

/*****************************************************************************/
/* WHEN EXPRESSION                                                           */
/*****************************************************************************/

WhenExpression::
WhenExpression()
{
}

WhenExpression::
WhenExpression(std::shared_ptr<SqlExpression> when)
    : when(when)
{
    surface = when->surface;
}

WhenExpression
WhenExpression::
parse(const std::string & str)
{
    ML::Parse_Context context(str, str.c_str(), str.length());
    auto result = parse(context, false /* allowUtf8 */);
    context.expect_eof("Unexpected characters at end of when expression");
    return result;
}

WhenExpression
WhenExpression::
parse(const char * str)
{
    return parse(string(str));
}

WhenExpression
WhenExpression::
parse(const Utf8String & str)
{
    ML::Parse_Context context(str.rawData(), str.rawData(), str.rawLength());
    auto result = parse(context, true /* allowUtf8 */);
    context.expect_eof("Unexpected characters at end of when expression");
    return result;
}

WhenExpression
WhenExpression::
parse(ML::Parse_Context & context, bool allowUtf8) {
    auto result = SqlExpression::parse(context, 10,  allowUtf8);
    return WhenExpression(result);
}

BoundWhenExpression
WhenExpression::
bind(SqlBindingScope & scope) const
{
    // First, check for an always true or always false expression
    if (when->isConstant()) {
        if (when->constantValue().isTrue()) {
            // keep everything, filter is a no-op
            auto filterInPlace = [] (MatrixNamedRow & row,
                                     const SqlRowScope & rowScope)
                {
                };

            return { filterInPlace, this };
        }
        else {
            // remove everything; filter is a clear
            auto filterInPlace = [] (MatrixNamedRow & row,
                                     const SqlRowScope & rowScope)
                {
                    row.columns.clear();
                };

            return { filterInPlace, this };
        }
    }

    // We need to bind the when in a special scope, that also knows about
    // the tuple we are filtering.
    SqlExpressionWhenScope & whenScope
        = static_cast<SqlExpressionWhenScope &>(scope);

    // Bind it in
    auto boundWhen = when->bind(whenScope);

    // Second, check for an expression that do not depend on tuples
    if (!whenScope.isTupleDependent) {
        // cerr << "not tuple dependent" << endl;
        auto filterInPlace = [=] (MatrixNamedRow & row,
                              const SqlRowScope & rowScope)
            {
                auto tupleScope = SqlExpressionWhenScope::getRowScope(rowScope, Date());
                if (!boundWhen(tupleScope, GET_LATEST).isTrue()) {
                    row.columns.clear();
                }
            };
        return { filterInPlace, this };
    }

    // Executing a when expression will filter the row by the expression,
    // applying it to each of the tuples
    auto filterInPlace = [=] (MatrixNamedRow & row,
                              const SqlRowScope & rowScope)
        {
            // Figure out which ones we keep.  We need to perform all of the
            // scanning before we extract any output, as we want the row
            // to remain intact (it's used by the rowScope) until we've
            // evaluated our when expression.

            std::vector<char> keep;  // not bool to avoid bitmap
            keep.reserve(row.columns.size());

            // How many columns do we output?
            size_t numOutput = 0;

            for (const auto & col: row.columns) {
                auto tupleScope
                    = SqlExpressionWhenScope
                    ::getRowScope(rowScope, std::get<2>(col));

                auto whenExpressionValue = boundWhen(tupleScope, GET_LATEST);
                bool keepThisCol = whenExpressionValue.isTrue();
                keep.emplace_back(keepThisCol);
                numOutput += keepThisCol;
            }

            // Now we create our output
            std::vector<std::tuple<ColumnName, CellValue, Date> > output;

            // If we keep them all, no need to copy
            if (numOutput == row.columns.size()) {
                return;
            }

            // Move the kept elements into the output
            for (unsigned i = 0;  i < row.columns.size();  ++i) {
                if (!keep[i])
                    continue;

                output.emplace_back(std::move(row.columns[i]));
            }
            
            // Swap them back in
            row.columns.swap(output);
        };

    return { filterInPlace, this };
}

Utf8String
WhenExpression::
print() const
{
    return "when("
        + when->print()
        + ")";
}

std::shared_ptr<SqlExpression>
WhenExpression::
transform(const TransformArgs & transformArgs) const
{
    throw HttpReturnException(400, "WhenExpression::transform()");
}

std::vector<std::shared_ptr<SqlExpression> >
WhenExpression::
getChildren() const
{
    std::vector<std::shared_ptr<SqlExpression> > result;
    result.push_back(when);
    return result;
}

bool
WhenExpression::
operator == (const WhenExpression & other) const
{
    return surface == other.surface;
}

UnboundEntities
WhenExpression::
getUnbound() const
{
   return when->getUnbound();
}

struct WhenExpressionDescription
    : public ValueDescriptionT<WhenExpression> {

    WhenExpressionDescription();

    virtual void parseJsonTyped(WhenExpression * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const WhenExpression * val,
                                JsonPrintingContext & context) const;
};

DEFINE_VALUE_DESCRIPTION_NS(WhenExpression, WhenExpressionDescription);

WhenExpressionDescription::
WhenExpressionDescription()
{
    setTypeName("SqlWhenExpression");
    documentationUri = "/doc/builtin/sql/WhenExpression.md";
}

void
WhenExpressionDescription::
parseJsonTyped(WhenExpression * val,
               JsonParsingContext & context) const
{
    *val = WhenExpression::parse(context.expectStringUtf8());
}

void
WhenExpressionDescription::
printJsonTyped(const WhenExpression * val,
               JsonPrintingContext & context) const
{
    context.writeStringUtf8(val->surface);
}


/******************************************************************************/
/* SELECT STATEMENT                                                           */
/******************************************************************************/

SelectStatement::
SelectStatement() :
    select(SelectExpression::STAR),
    when(WhenExpression::TRUE),
    where(SelectExpression::TRUE),
    having(SelectExpression::TRUE),
    rowName(SqlExpression::parse("rowPath()")),
    offset(0),
    limit(-1)
{
    //TODO - avoid duplication of default values
}

SelectStatement
SelectStatement::
parse(const Utf8String& body)
{
    return parse(body.rawString());
}

SelectStatement
SelectStatement::
parse(const std::string& body)
{
    ML::Parse_Context context(body, body.c_str(), body.length());

    const bool acceptUtf8 = true;

    SelectStatement stm = parse(context, acceptUtf8);

    context.expect_eof();

    return std::move(stm);
}

SelectStatement
SelectStatement::
parse(const char * body)
{
    return parse(string(body));
}

SelectStatement
SelectStatement::parse(ML::Parse_Context& context, bool acceptUtf8)
{
    ML::Parse_Context::Hold_Token token(context);

    SelectStatement statement;

    if (matchKeyword(context, "SELECT ")) {
        statement.select = SelectExpression::parse(context, acceptUtf8);
    }
    else {
        context.exception("Expected SELECT");
    }

    bool needDefaultRowName = false;
    if (matchKeyword(context, "NAMED ")) {
        statement.rowName = SqlExpression::parse(context, 10, acceptUtf8);
        skip_whitespace(context);
    }
    else {
        //default name is "rowPath()" if there is a dataset and "result" if there is none
        //we need to check if there is a FROM first.
        needDefaultRowName = true;
    }

    if (matchKeyword(context, "FROM ")) {
        statement.from = TableExpression::parse(context, 10, acceptUtf8);
        skip_whitespace(context);
        if (needDefaultRowName)
            statement.rowName = SqlExpression::parse("rowPath()");
    }
    else {
        statement.from = std::make_shared<NoTable>();
        if (needDefaultRowName)
            statement.rowName = SqlExpression::parse("'result'");
    }
    
    if (matchKeyword(context, "WHEN ")) {
        statement.when = WhenExpression(SqlExpression::parse(context, 10, acceptUtf8));
        skip_whitespace(context);
    }
    else {
        statement.when = WhenExpression::parse("true");
    }

    if (matchKeyword(context, "WHERE ")) {
        statement.where = SqlExpression::parse(context, 10, acceptUtf8);
        skip_whitespace(context);
    }
    else {
        statement.where = SqlExpression::parse("true");
    }

    if (matchKeyword(context, "GROUP BY ")) {
        statement.groupBy = TupleExpression::parse(context, acceptUtf8);
        skip_whitespace(context);
    }

    if (matchKeyword(context, "HAVING ")) {
        statement.having = SqlExpression::parse(context, 10, acceptUtf8);
        skip_whitespace(context);
    }
    else {
        statement.having = SqlExpression::parse("true");
    }

    if (matchKeyword(context, "ORDER BY ")) {
        statement.orderBy = OrderByExpression::parse(context, acceptUtf8);
        skip_whitespace(context);
    }

    if (matchKeyword(context, "LIMIT ")) {
        statement.limit = context.expect_unsigned_long_long();
        skip_whitespace(context);
    }
    else {
        statement.limit = -1;
    }

    if (matchKeyword(context, "OFFSET ")) {
        statement.offset = context.expect_unsigned_long_long();
        skip_whitespace(context);
    }
    else {
        statement.offset = 0;
    }

    statement.surface = ML::trim(token.captured());

    skip_whitespace(context);

    //cerr << jsonEncode(statement) << endl;
    return std::move(statement);
}

Utf8String
SelectStatement::
print() const
{
    return select.print() + 
        rowName->print() +
        from->print() +
        when.print() +
        where->print() +
        orderBy.print() +
        groupBy.print() +
        having->print();
}

UnboundEntities
SelectStatement::
getUnbound() const
{
    UnboundEntities result;

    result.merge(select.getUnbound());
    result.merge(from->getUnbound());
    result.merge(when.getUnbound());
    result.merge(where->getUnbound());
    result.merge(orderBy.getUnbound());
    result.merge(groupBy.getUnbound());
    result.merge(having->getUnbound());
    result.merge(rowName->getUnbound());

    return result;
}

DEFINE_STRUCTURE_DESCRIPTION(SelectStatement);


SelectStatementDescription::
SelectStatementDescription()
{
    addField("select",  &SelectStatement::select,  "SELECT clause");
    addField("named",   &SelectStatement::rowName, "NAMED clause");
    addField("from",    &SelectStatement::from,    "FROM clause");
    addField("when",    &SelectStatement::when,    "WHEN clause");
    addField("where",   &SelectStatement::where,   "WHERE clause");
    addField("orderBy", &SelectStatement::orderBy, "ORDER BY clause");
    addField("groupBy", &SelectStatement::groupBy, "GROUP BY clause");
    addField("having",  &SelectStatement::having,  "HAVING clause");
    addField("offset",  &SelectStatement::offset,  "OFFSET clause", (ssize_t)0);
    addField("limit",   &SelectStatement::limit,   "LIMIT clause", (ssize_t)-1);
}

struct InputQueryDescription
    : public ValueDescriptionT<InputQuery> {

    InputQueryDescription();

    virtual void parseJsonTyped(InputQuery * val,
                                JsonParsingContext & context) const;

    virtual void printJsonTyped(const InputQuery * val,
                                JsonPrintingContext & context) const;
};

void
InputQueryDescription::
parseJsonTyped(InputQuery * val,
               JsonParsingContext & context) const
{
    if (context.isString())
        val->stm = make_shared<SelectStatement>(SelectStatement::parse(context.expectStringUtf8()));
    else if (context.isObject()) {
        Json::Value v = context.expectJson();
        SelectStatement stm;
        SelectStatementDescription desc;
        desc.parseJson(&stm, context);
        val->stm = make_shared<SelectStatement>(std::move(stm));
        val->stm->surface = v.toStringNoNewLine();
    }
}
 
void
InputQueryDescription::
printJsonTyped(const InputQuery * val,
               JsonPrintingContext & context) const
{
    if (!val->stm)
        context.writeNull();
    else {
        SelectStatementDescription desc;
        desc.printJsonTyped(val->stm.get(), context);
    }
}

DEFINE_VALUE_DESCRIPTION_NS(InputQuery, InputQueryDescription);

InputQueryDescription::
InputQueryDescription()
{
    setTypeName("InputQuery");
    documentationUri = "/doc/builtin/procedures/InputQuery.md";
}

} // namespace MLDB
} // namespace Datacratic

