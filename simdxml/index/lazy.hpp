// Query-driven lazy parser — only index tags relevant to a specific XPath query.
//
// Ported from Rust `index/lazy.rs`. The parser still scans all `<` positions
// (can't avoid this), but only builds full index entries for tags whose names
// are in the "interesting" set plus their ancestors. Text ranges are only
// captured under interesting elements. Can skip 70-90% of index construction
// for selective queries on large XML.
#pragma once

#include "../error.hpp"
#include "xml_index.hpp"

#include <span>
#include <string>
#include <unordered_set>

namespace rai::xml {

/// Parse XML, only indexing tags with names in `interesting_names` and their
/// ancestors. Falls back to full parsing if `interesting_names` is empty.
[[nodiscard]] Result<XmlIndex>
parse_for_query(std::span<std::byte const> input,
                std::unordered_set<std::string> const& interesting_names);

}  // namespace rai::xml