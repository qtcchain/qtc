// Package errs holds the one error type the service surfaces: a kind (which HTTP class it maps to)
// plus a stable machine code and a human message. Secrets never go into messages.
package errs

import "fmt"

type Kind int

const (
	BadRequest      Kind = iota // 400
	Unauthorized                // 401
	ScopeDenied                 // 403
	NotFound                    // 404
	Conflict                    // 409 (policy_denied, security_alert, mempool_reject, destination_not_allowed)
	Frozen                      // 423
	NodeUnavailable             // 503
	NodeError                   // 502 (RPC error from the node)
	Internal                    // 500
)

type Error struct {
	Kind    Kind
	Code    string
	Message string
	RPCCode int // set for NodeError
}

func (e *Error) Error() string {
	if e.Message == "" {
		return e.Code
	}
	return e.Code + ": " + e.Message
}

func New(kind Kind, code, format string, args ...any) *Error {
	return &Error{Kind: kind, Code: code, Message: fmt.Sprintf(format, args...)}
}

func Bad(code, format string, args ...any) *Error { return New(BadRequest, code, format, args...) }
func Scope(scope string) *Error                   { return New(ScopeDenied, "scope_denied", "%s", scope) }
func Froze(reason string) *Error                  { return New(Frozen, "frozen", "%s", reason) }
func Unavailable(format string, args ...any) *Error {
	return New(NodeUnavailable, "node_unavailable", format, args...)
}
func NotFoundErr(code, format string, args ...any) *Error {
	return New(NotFound, code, format, args...)
}
func Conflicting(code, format string, args ...any) *Error {
	return New(Conflict, code, format, args...)
}

// RPC wraps a JSON-RPC error from the node.
func RPC(code int, msg string) *Error {
	return &Error{Kind: NodeError, Code: "node_error", Message: msg, RPCCode: code}
}

// As returns the *Error inside err, or nil.
func As(err error) *Error {
	if e, ok := err.(*Error); ok {
		return e
	}
	return nil
}

// IsRPC reports whether err is a node RPC error with the given JSON-RPC code.
func IsRPC(err error, code int) bool {
	e := As(err)
	return e != nil && e.Kind == NodeError && e.RPCCode == code
}
