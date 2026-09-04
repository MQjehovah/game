#include "neon/scene/data_table.hpp"

#include <algorithm>
#include <string>

#include "neon/core/json.hpp"
#include "neon/core/result.hpp"

namespace neon::scene {

// Loads a data table from a JSON document whose root is an ARRAY of row objects.
// `typeName` must be registered in the TypeRegistry (so its type-erased
// fromJson can validate each row). Returns Ok with the validated table, or Err
// with a human-readable reason (bad JSON, unknown type, / or one or more rows
// that failed validation — the count is shown but the valid rows are kept).
core::Result<DataTable> LoadDataTable(const std::string& typeName,
                                      const std::string& jsonText) {
    DataTable out;
    out.typeName = typeName;

    std::string parseErr;
    core::Json root = core::Json::Parse(jsonText, &parseErr);
    if (!parseErr.empty())
        return core::Result<DataTable>::Err("data: JSON parse error: " + parseErr);
    if (!root.IsArray())
        return core::Result<DataTable>::Err("data: table '" + typeName + "' must be a JSON array");

    const TypeInfo* info = TypeRegistry::Find(typeName);
    if (!info || !info->fromJson)
        return core::Result<DataTable>::Err("data: unknown table type '" + typeName + "'");

    int rejected = 0;
    for (size_t i = 0; i < root.Size(); ++i) {
        const core::Json* row = root.At(i);
        if (!row || !row->IsObject() || !info->normalize) {
            ++rejected;
            continue;
        }
        core::Json normalized = info->normalize(*row);
        if (normalized.IsNull()) {
            ++rejected;
            continue;
        }
        out.rows.push_back(std::move(normalized));
    }

    if (rejected > 0)
        return core::Result<DataTable>::Err(
            "data: table '" + typeName + "': " + std::to_string(rejected) +
            " row(s) failed validation (kept " + std::to_string(out.rows.size()) + ")");
    return core::Result<DataTable>::Ok(std::move(out));
}

} // namespace neon::scene
