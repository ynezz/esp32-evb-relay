package format

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strings"
	"text/tabwriter"
)

func Output(w io.Writer, data any, format string) error {
	if w == nil {
		return errors.New("output writer is required")
	}

	switch strings.ToLower(strings.TrimSpace(format)) {
	case JSON:
		return outputJSON(w, data)
	case Table:
		return outputTable(w, data)
	case Plain:
		return outputPlain(w, data)
	default:
		return fmt.Errorf("unsupported human format %q: expected table, json, or plain", format)
	}
}

func outputJSON(w io.Writer, data any) error {
	encoder := json.NewEncoder(w)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	return encoder.Encode(data)
}

func outputTable(w io.Writer, data any) error {
	table, err := tableOutputFor(data)
	if err != nil {
		return err
	}

	if err := validateTable(table); err != nil {
		return err
	}

	tw := tabwriter.NewWriter(w, 0, 0, 2, ' ', 0)
	if _, err := fmt.Fprintln(tw, strings.Join(table.Headers, "\t")); err != nil {
		return err
	}

	for _, row := range table.Rows {
		if _, err := fmt.Fprintln(tw, strings.Join(row, "\t")); err != nil {
			return err
		}
	}

	return tw.Flush()
}

func outputPlain(w io.Writer, data any) error {
	lines, err := plainOutputFor(data)
	if err != nil {
		return err
	}

	for _, line := range lines {
		if _, err := fmt.Fprintln(w, line); err != nil {
			return err
		}
	}

	return nil
}

func tableOutputFor(data any) (TableData, error) {
	switch value := data.(type) {
	case nil:
		return TableData{}, errors.New("table output requires data")
	case TableData:
		return value, nil
	case *TableData:
		if value == nil {
			return TableData{}, errors.New("table output requires data")
		}
		return *value, nil
	case TableProvider:
		return value.TableOutput()
	default:
		return TableData{}, fmt.Errorf("table output is not available for %T", data)
	}
}

func plainOutputFor(data any) ([]string, error) {
	switch value := data.(type) {
	case nil:
		return nil, nil
	case string:
		return []string{value}, nil
	case []string:
		return value, nil
	case PlainProvider:
		return value.PlainOutput()
	default:
		return nil, fmt.Errorf("plain output is not available for %T", data)
	}
}

func validateTable(table TableData) error {
	if len(table.Headers) == 0 {
		return errors.New("table output requires at least one header")
	}

	for index, row := range table.Rows {
		if len(row) != len(table.Headers) {
			return fmt.Errorf(
				"table row %d has %d columns; expected %d",
				index,
				len(row),
				len(table.Headers),
			)
		}
	}

	return nil
}
