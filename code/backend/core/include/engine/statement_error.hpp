#pragma once

#include <stdexcept>

namespace engine {

// A statement that fails while an expression is being evaluated: a scalar subquery that returns two rows, an error inside a subquery. The
// evaluators hand back a truth value (or a text) and have no way to carry an error through a comparison inside a scan, so they throw this and
// execute_with_s turns it into the statement's error (the statement has changed nothing yet: it is thrown while rows are being picked).
struct StatementError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace engine
