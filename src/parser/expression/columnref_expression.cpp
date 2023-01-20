#include "duckdb/parser/expression/columnref_expression.hpp"

#include "duckdb/common/field_writer.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

ColumnRefExpression::ColumnRefExpression(string column_name, string table_name)
    : ColumnRefExpression(table_name.empty() ? vector<string> {std::move(column_name)}
                                             : vector<string> {std::move(table_name), std::move(column_name)}) {
}

ColumnRefExpression::ColumnRefExpression(string column_name)
    : ColumnRefExpression(vector<string> {std::move(column_name)}) {
}

ColumnRefExpression::ColumnRefExpression(vector<string> column_names_p)
    : ParsedExpression(ExpressionType::COLUMN_REF, ExpressionClass::COLUMN_REF),
      column_names(std::move(column_names_p)) {
#ifdef DEBUG
	for (auto &col_name : column_names) {
		D_ASSERT(!col_name.empty());
	}
#endif
	VerifyQualification();
}

void ColumnRefExpression::VerifyQualification() const {
	// This is the max amount of qualification possible
	D_ASSERT(column_names.size() <= (idx_t)ColumnQualification::QUALIFICATION_ENUM_SIZE);
}

bool ColumnRefExpression::IsQualified(ColumnQualification qualifier) const {
	VerifyQualification();
	const auto max_qualification = (uint8_t)(column_names.size() - 1);
	return max_qualification >= (uint8_t)qualifier;
}

const string &ColumnRefExpression::GetColumnName() const {
	VerifyQualification();
	return column_names.back();
}

const vector<string> &ColumnRefExpression::GetColumnNames() const {
	return column_names;
}

idx_t ColumnRefExpression::GetIndexOfQualification(ColumnQualification qualifier) const {
	VerifyQualification();

	D_ASSERT(IsQualified(qualifier));

	// qualifier = 0 (NAME) | size = 1 (name) -> return 0;
	// qualifier = 0 (NAME) | size = 2 (tbl.name) -> return 1;
	// qualifier = 1 (TABLE) | size = 2 (tbl.name) -> return 0;
	// qualifier = 1 (TABLE) | size = 3 (schema.tbl.name) -> return 1;

	const auto max_qualification = (uint8_t)(column_names.size() - 1);
	return max_qualification - (uint8_t)qualifier;
}

const string &ColumnRefExpression::GetQualificationName(ColumnQualification qualifier) const {
	auto index = GetIndexOfQualification(qualifier);
	return column_names[index];
}

const string &ColumnRefExpression::GetTableName() const {
	D_ASSERT(IsQualified());
	return GetQualificationName(ColumnQualification::TABLE);
}

string ColumnRefExpression::GetName() const {
	return !alias.empty() ? alias : column_names.back();
}

string ColumnRefExpression::ToString() const {
	string result;
	for (idx_t i = 0; i < column_names.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += KeywordHelper::WriteOptionallyQuoted(column_names[i]);
	}
	return result;
}

bool ColumnRefExpression::Equal(const ColumnRefExpression *a, const ColumnRefExpression *b) {
	if (a->column_names.size() != b->column_names.size()) {
		return false;
	}
	for (idx_t i = 0; i < a->column_names.size(); i++) {
		auto lcase_a = StringUtil::Lower(a->column_names[i]);
		auto lcase_b = StringUtil::Lower(b->column_names[i]);
		if (lcase_a != lcase_b) {
			return false;
		}
	}
	return true;
}

hash_t ColumnRefExpression::Hash() const {
	hash_t result = ParsedExpression::Hash();
	for (auto &column_name : column_names) {
		auto lcase = StringUtil::Lower(column_name);
		result = CombineHash(result, duckdb::Hash<const char *>(lcase.c_str()));
	}
	return result;
}

unique_ptr<ParsedExpression> ColumnRefExpression::Copy() const {
	auto copy = make_unique<ColumnRefExpression>(column_names);
	copy->CopyProperties(*this);
	return std::move(copy);
}

void ColumnRefExpression::Serialize(FieldWriter &writer) const {
	writer.WriteList<string>(column_names);
}

unique_ptr<ParsedExpression> ColumnRefExpression::Deserialize(ExpressionType type, FieldReader &reader) {
	auto column_names = reader.ReadRequiredList<string>();
	auto expression = make_unique<ColumnRefExpression>(std::move(column_names));
	return std::move(expression);
}

} // namespace duckdb
