#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <iostream>
#include <sstream>

// Forward declaration of global debug flag
extern bool debug_log;

// Debug stream class that only outputs when debug_log is true
class DebugStream {
private:
    std::ostringstream buffer;

public:
    template<typename T>
    DebugStream& operator<<(const T& data) {
        if (debug_log) {
            buffer << data;
            std::cerr << buffer.str();
            buffer.str(""); // Clear the buffer after output
        }
        return *this;
    }

    // Support for std::endl and other manipulators
    DebugStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (debug_log) {
            buffer << manip;
            std::cerr << buffer.str();
            buffer.str(""); // Clear the buffer after output
        }
        return *this;
    }
};

// Global debug stream instance
extern DebugStream dbg;

#endif // DEBUG_HPP
