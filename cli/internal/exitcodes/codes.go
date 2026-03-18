package exitcodes

import "errors"

type Code int

const (
	Success Code = iota
	GeneralError
	NetworkError
	AuthError
	NotFound
	BadArgument
	StateError
	HardwareUnavailable
)

type coder interface {
	ExitCode() Code
}

type codedError struct {
	code Code
	err  error
}

func (e codedError) Error() string {
	return e.err.Error()
}

func (e codedError) Unwrap() error {
	return e.err
}

func (e codedError) ExitCode() Code {
	return e.code
}

func Wrap(code Code, err error) error {
	if err == nil {
		return nil
	}

	var existing coder
	if errors.As(err, &existing) {
		return err
	}

	return codedError{code: code, err: err}
}

func FromError(err error) Code {
	if err == nil {
		return Success
	}

	var existing coder
	if errors.As(err, &existing) {
		return existing.ExitCode()
	}

	return GeneralError
}
