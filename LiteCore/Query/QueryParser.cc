//
//  QueryParser.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "QueryParser.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include <utility>
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore {


    #define TABLECOUNT(TABLE)   (sizeof(TABLE) / sizeof((TABLE)[0]))

    // Relational operators, appearing as dictionary keys, e.g. {"$eq": 42}
    // Used by parseTerm(); the 'type' numbers match cases in a switch statement.
    struct relationalEntry {const char *op; const char *sqlOp; int type;};
    static const relationalEntry kRelationals[] = {
        {"$eq",     " = "},
        {"$ne",     " <> "},
        {"$lt",     " < "},
        {"$lte",    " <= "},
        {"$le",     " <= "},
        {"$gt",     " > "},
        {"$gte",    " >= "},
        {"$ge",     " >= "},
        {"$like",   " LIKE "},
        {"$type",   nullptr,    1},
        {"$exists", nullptr,    2},
        {"$in",     " IN ",     3},
        {"$nin",    " NOT IN ", 3},
        {"$size",   nullptr,    4},
        {"$all",    nullptr,    5},
        {"$any",    nullptr,    6},
        {"$elemMatch",nullptr,  7},
        {"$match",  " MATCH ",  8},
    };

    // Names of Fleece types, indexed by fleece::valueType. Used with "$type" operator.
    static const char* const kTypeNames[] = {
        "null", "boolean", "number", "string", "blob", "array", "object"
    };
    
    
    static void fail() {
        error::_throw(error::InvalidQuery);
    }


    static const Value* mustExist(const Value *v) {
        if (!v)
            fail();
        return v;
    }

    static const Array* mustBeArray(const Value *v) {
        if (!v)
            return nullptr;
        const Array *a = v->asArray();
        if (!a)
            fail();
        return a;
    }

    static const Dict* mustBeDict(const Value *v) {
        if (!v)
            return nullptr;
        const Dict *d = v->asDict();
        if (!d)
            fail();
        return d;
    }

    // If the dict has a key starting with '$', returns it and its value; else nulls.
    static pair<slice,const Value*> getSpecialKey(const Value *val) {
        for (Dict::iterator i(val->asDict()); i; ++i) {
            slice key = i.key()->asString();
            if (key.size > 0 && key[0] == '$') {
                return {key, i.value()};
            }
        }
        return {};
    }


    // Utility that writes its 'word' string to the output every time next() is called but the 1st.
    class Delimiter {
    public:
        Delimiter(ostream &out, const string &word)
        :_out(out), _word(word)
        { }

        void next() {
            if (_first)
                _first = false;
            else
                _out << _word;
        }
    private:
        ostream &_out;
        string _word;
        bool _first {true};
    };


    // Appends two property-path strings.
    static string appendPaths(const string &parent, string child) {
        if (child[0] == '$') {
            if (child[1] == '.')
                child = child.substr(2);
            else
                child = child.substr(1);
        }
        if (parent.empty())
            return child;
        else if (child[0] == '[')
            return parent + child;
        else
            return parent + "." + child;

    }


#pragma mark - QUERY PARSER:


    void QueryParser::parse(const Value *whereExpression, const Value *sortExpression) {
        if (whereExpression)
            parsePredicate(whereExpression);
        parseSort(sortExpression);
    }


    void QueryParser::parseJSON(slice whereJSON, slice sortJSON) {
        const Value *whereValue = nullptr, *sortValue = nullptr;
        alloc_slice whereFleece, sortFleece;
        if (whereJSON) {
            whereFleece = JSONConverter::convertJSON(whereJSON);
            whereValue = Value::fromTrustedData(whereFleece);
        }
        if (sortJSON) {
            sortFleece = JSONConverter::convertJSON(sortJSON);
            sortValue = Value::fromTrustedData(sortFleece);
        }
        parse(whereValue, sortValue);
    }


    // Parses a boolean-valued expression, usually the top level of a query.
    void QueryParser::parsePredicate(const Value *q) {
        const Dict *query = mustBeDict(mustExist(q));
        auto special = getSpecialKey(query);
        if (!special.first.buf) {
            // No special operator; interpret each key as a property path with implicit 'and':
            Delimiter d(_sql, " AND ");
            for (Dict::iterator i(query); i; ++i) {
                d.next();
                parseTerm(i.key()->asString(), i.value());
            }
        } else if (special.first == "$and"_sl) {
            writeBooleanExpr(special.second, " AND ");
        } else if (special.first == "$or"_sl) {
            writeBooleanExpr(special.second, " OR ");
        } else if (special.first == "$nor"_sl) {
            _sql << "NOT (";
            writeBooleanExpr(special.second, " OR ");
            _sql << ")";
        } else if (special.first == "$not"_sl) {
            auto terms = mustBeArray(special.second);
            if (terms->count() != 1)
                fail();
            _sql << "NOT (";
            parsePredicate(terms->get(0));
            _sql << ")";
        }
    }


    // Writes a series of terms separated by AND or OR operators.
    void QueryParser::writeBooleanExpr(const Value *terms, const char *op) {
        Delimiter d(_sql, op);
        for (Array::iterator i(mustBeArray(mustExist(terms))); i; ++i) {
            d.next();
            parsePredicate(i.value());
        }
    }


    // Returns the type of relation found in a value, e.g. `$eq`
    static const relationalEntry* findRelation(const Value* &value) {
        // First determine the comparison operation:
        slice op;
        auto special = getSpecialKey(value);
        if (special.second) {
            op = special.first;
            value = special.second;
        } else {
            if (value->type() == kDict)
                return nullptr;
            op = "$eq"_sl;
        }

        // Look up `op` in the kRelationals table:
        const relationalEntry *rel = nullptr;
        for (unsigned i = 0; i < TABLECOUNT(kRelationals); ++i) {
            if (op == slice(kRelationals[i].op)) {
                rel = &kRelationals[i];
                break;
            }
        }
        if (!rel)
            fail();
        return rel;
    }


    // Parses a key/value mapping, like `"x": {"$gt": 5}"
    void QueryParser::parseTerm(slice key, const Value *value) {
        // Process the relation by type:
        const relationalEntry *rel = findRelation(value);

        if (!rel) {
            parseSubPropertyTerm(key, value->asDict());
            return;
        }

        switch (rel->type) {
            case 0:     // Comparison operator like $eq, $lt, etc.
                writePropertyGetter("fl_value", key);
                _sql << rel->sqlOp;
                writeLiteral(value);
                break;
            case 1: {   // $type
                writePropertyGetter("fl_type", key);
                _sql << "=";
                int typeCode = -1;
                slice typeName = value->asString();
                for (int i = 0; i < TABLECOUNT(kTypeNames); ++i) {
                    if (typeName == slice(kTypeNames[i])) {
                        typeCode = i;
                        break;
                    }
                }
                if (typeCode < 0)
                    fail();
                _sql << typeCode;
                break;
            }
            case 2:     // $exists
                if (!value->asBool())
                    _sql << "NOT ";
                writePropertyGetter("fl_exists", key);
                break;
            case 3: {   // $in, $nin
                writePropertyGetter("fl_value", key);
                _sql << rel->sqlOp << "(";
                Delimiter d(_sql, ", ");
                for (Array::iterator i(mustBeArray(value)); i; ++i) {
                    d.next();
                    writeLiteral(i.value());
                }
                _sql << ")";
                break;
            }
            case 4:     // $size
                writePropertyGetter("fl_count", key);
                _sql << "=";
                writeLiteral(value);
                break;
            case 5:     // $all
            case 6: {   // $any
                writePropertyGetterLeftOpen("fl_contains", key);
                _sql << ((rel->type == 5) ? ", 1" : ", 0");
                for (Array::iterator i(mustBeArray(value)); i; ++i) {
                    _sql << ", ";
                    writeLiteral(i.value());
                }
                _sql << ")";
                break;
            }
            case 7:     // $elemMatch
                parseElemMatch(key, value);
                break;
            case 8:
                parseFTSMatch(key, value);
                break;
            default:
                Assert(false, "invalid type in kRelationals");
        }
    }


    // Parses a nested predicate inside a property.
    void QueryParser::parseSubPropertyTerm(slice property, const Dict *value) {
        // Append this property to _propertyPath:
        auto savedPropertyPath = _propertyPath;
        _propertyPath = appendPaths(_propertyPath, (string)property);
        _sql << "(";
        parsePredicate(value);
        _sql << ")";
        // On exit, restore _propertyPath:
        _propertyPath = savedPropertyPath;
    }


    // Writes a SQL string containing a Fleece path to the property.
    void QueryParser::writePropertyPathString(slice property) {
        auto path = appendPaths(_propertyPath, (string)property);
        writeSQLString(_sql, slice(path));
    }


    // Writes a call to a Fleece SQL function, without the closing ")".
    void QueryParser::writePropertyGetterLeftOpen(const char *fn, slice property) {
        _sql << fn << "(" << _jsonColumnName << ", ";
        writePropertyPathString(property);
    }


    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(const char *fn, slice property) {
        if (property == "_id"_sl) {
            if (strcmp(fn, "fl_value") != 0)
                fail();
            _sql << "key";
        } else if (property == "_sequence"_sl) {
            if (strcmp(fn, "fl_value") != 0)
                fail();
            _sql << "sequence";
        } else {
            writePropertyGetterLeftOpen(fn, property);
            _sql << ")";
        }
    }


    /*static*/ string QueryParser::propertyGetter(slice property, const char *sqlColumn) {
        QueryParser qp("XXX", sqlColumn);
        qp.writePropertyGetter("fl_value", property);
        return qp.whereClause();
    }


    // Writes a Fleece value as a SQL literal.
    void QueryParser::writeLiteral(const Value *literal) {
        switch (literal->type()) {
            case kNumber: {
                alloc_slice str = literal->toString();
                _sql << string((const char*)str.buf, str.size);
                break;
            }
            case kBoolean:
                _sql << (literal->asBool() ? "1" : "0");    // SQL doesn't have true/false
                break;
            case kString:
                writeSQLString(literal->asString());
                break;
            case kArray: {
                // An single-item array containing an integer or string is a placeholder/binding.
                auto a = literal->asArray();
                if (a->count() != 1)
                    fail();
                const Value *ident = a->get(0);
                if (ident->isInteger()) {
                    _sql << ":_" << ident->asInt();
                } else {
                    slice str = ident->asString();
                    if (!str.buf)
                        fail();
                    // TODO: Check that str is a valid identifier
                    _sql << ":_" << (string)str;
                }
                break;
            }
            default:
                fail();
        }
    }


    // Writes a string with SQL quoting (inside apostrophes, doubling contained apostrophes.)
    /*static*/ void QueryParser::writeSQLString(std::ostream &out, slice str) {
        out << "'";
        bool simple = true;
        for (unsigned i = 0; i < str.size; i++) {
            if (str[i] == '\'') {
                simple = false;
                break;
            }
        }
        if (simple) {
            out.write((const char*)str.buf, str.size);
        } else {
            for (unsigned i = 0; i < str.size; i++) {
                if (str[i] == '\'')
                    out.write("''", 2);
                else
                    out.write((const char*)&str[i], 1);
            }
        }
        out << "'";
   }


#pragma mark - ELEMMATCH:


    // Parses an "$elemMatch" expression
    void QueryParser::parseElemMatch(slice property, const fleece::Value *match) {
        // Query the "table":
        _sql << "EXISTS (SELECT 1 FROM ";
        writePropertyGetter("fl_each", property);
        _sql << " WHERE ";
        parseElemMatchTerm("fl_each", match);
        _sql << ")";
    }


    // Parses a key/value mapping within an $elemMatch
    void QueryParser::parseElemMatchTerm(const string &table, const Value *value) {
        // Process the relation by type:
        const relationalEntry *rel = findRelation(value);

        if (!rel) {
            fail(); //TODO: IMPLEMENT
            //parseSubPropertyTerm(key, value->asDict());
            return;
        }

        switch (rel->type) {
            case 0:     // Comparison operator like $eq, $lt, etc.
                _sql << table << ".value";
                _sql << rel->sqlOp;
                writeLiteral(value);
                break;
            case 1: {   // $type
                _sql << table << ".type";
                _sql << "=";
                int typeCode = -1;
                slice typeName = value->asString();
                for (int i = 0; i < TABLECOUNT(kTypeNames); ++i) {
                    if (typeName == slice(kTypeNames[i])) {
                        typeCode = i;
                        break;
                    }
                }
                if (typeCode < 0)
                    fail();
                _sql << typeCode;
                break;
            }
            case 2:     // $exists
                if (!value->asBool())
                    _sql << "NOT ";
                _sql << "(" << table << ".type >= 0)";
                break;
            case 3: {   // $in, $nin
                _sql << table << ".value";
                _sql << rel->sqlOp << "(";
                Delimiter d(_sql, ", ");
                for (Array::iterator i(mustBeArray(value)); i; ++i) {
                    d.next();
                    writeLiteral(i.value());
                }
                _sql << ")";
                break;
            }
            case 4:     // $size
                _sql << "count(" << table << ".*)";
                _sql << "=";
                writeLiteral(value);
                break;
            case 5:     // $all
            case 6: {   // $any
                fail(); //TODO: IMPLEMENT
//                writePropertyGetterLeftOpen("fl_contains", table);
//                _sql << ((rel->type == 5) ? ", 1" : ", 0");
//                for (Array::iterator i(mustBeArray(value)); i; ++i) {
//                    _sql << ", ";
//                    writeLiteral(i.value());
//                }
//                _sql << ")";
                break;
            }
            case 7:     // $elemMatch
                fail(); //TODO: IMPLEMENT
                //parseElemMatch(table, value);
                break;
            default:
                Assert(false, "invalid type in kRelationals");
        }
    }


#pragma mark - FULL-TEXT-SEARCH MATCH:


    size_t QueryParser::FTSPropertyIndex(const string &propertyPath) {
        auto i = find(_ftsProperties.begin(), _ftsProperties.end(), propertyPath);
        if (i == _ftsProperties.end())
            return 0;
        return i - _ftsProperties.begin() + 1;
    }


    void QueryParser::parseFTSMatch(slice property, const fleece::Value *match) {
        // The FTS index is a separate (virtual) table. Get the table name and add it to FROM:
        auto propertyPath = appendPaths(_propertyPath, (string)property);
        size_t ftsTableNo = FTSPropertyIndex(propertyPath);
        if (ftsTableNo == 0) {
            _ftsProperties.push_back(propertyPath);
            ftsTableNo = _ftsProperties.size();
        }

        // Write the match expression (using an implicit join):
        _sql << "(FTS" << ftsTableNo << ".text MATCH ";
        writeLiteral(match);
        _sql << " AND FTS" << ftsTableNo << ".rowid = " << _tableName << ".sequence)";
    }


    string QueryParser::fromClause() {
        stringstream from;
        from << _tableName;
        unsigned ftsTableNo = 0;
        for (auto propertyPath : _ftsProperties) {
            from << ", \"" << _tableName << "::" << propertyPath << "\" AS FTS" << ++ftsTableNo;
        }
        return from.str();
    }


    vector<string> QueryParser::ftsTableNames() const {
        vector<string> names;
        for (auto propertyPath : _ftsProperties) {
            stringstream s;
            s << "\"" << _tableName << "::" << propertyPath << "\"";
            names.push_back(s.str());
        }
        return names;
    }


#pragma mark - SORTING:


    void QueryParser::parseSort(const Value *expr) {
        if (!expr) {
            _sortSQL << "key";
        } else switch (expr->type()) {
            case kString:
                writeOrderBy(expr);
                break;
            case kArray: {
                Delimiter d(_sortSQL, ", ");
                for (Array::iterator it(expr->asArray()); it; ++it) {
                    d.next();
                    writeOrderBy(it.value());
                }
                break;
            }
            default:
                fail();
        }
    }


    void QueryParser::writeOrderBy(const Value *property) {
        slice str = property->asString();
        if (str.size < 1)
            fail();

        if (FTSPropertyIndex((string)str) > 0) {
            writeOrderByFTSRank(str);
            return;
        }
        
        bool ascending = true;
        char prefix = str.peekByte();
        if (prefix == '-' || prefix == '+') {
            ascending = (prefix == '+');
            str.readByte();
        }
        
        if (str == "_id"_sl)
            _sortSQL << "key";
        else if (str == "_sequence"_sl)
            _sortSQL << "sequence";
        else
            _sortSQL << propertyGetter(str);
        if (!ascending)
            _sortSQL << " DESC";
    }


    void QueryParser::writeOrderByFTSRank(slice property) {
        _sortSQL << "rank(matchinfo(\"" << _tableName << "::" << (string)property << "\")) DESC";
    }

}