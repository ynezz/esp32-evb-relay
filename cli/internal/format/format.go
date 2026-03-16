package format

import (
	"fmt"
	"strings"
)

const (
	Table = "table"
	JSON  = "json"
	Plain = "plain"
	TOON  = "toon"
)

func Resolve(requested string, robot bool) (string, error) {
	normalized := strings.ToLower(strings.TrimSpace(requested))

	if robot {
		if normalized == "" {
			return TOON, nil
		}
		if normalized == JSON {
			return JSON, nil
		}
		return "", fmt.Errorf("invalid robot format %q: only json is allowed as an explicit override", requested)
	}

	if normalized == "" {
		return Table, nil
	}

	switch normalized {
	case Table, JSON, Plain:
		return normalized, nil
	default:
		return "", fmt.Errorf("unsupported format %q: expected table, json, or plain", requested)
	}
}

func HumanFormats() []string {
	return []string{Table, JSON, Plain}
}

type TableData struct {
	Headers []string
	Rows    [][]string
}

type TableProvider interface {
	TableOutput() (TableData, error)
}

type PlainProvider interface {
	PlainOutput() ([]string, error)
}

type HumanData struct {
	Table TableData
	Plain []string
}

func (d HumanData) TableOutput() (TableData, error) {
	return d.Table, nil
}

func (d HumanData) PlainOutput() ([]string, error) {
	if d.Plain != nil {
		return d.Plain, nil
	}

	if len(d.Table.Headers) <= 1 {
		lines := make([]string, 0, len(d.Table.Rows))
		for _, row := range d.Table.Rows {
			if len(row) == 0 {
				lines = append(lines, "")
				continue
			}
			lines = append(lines, row[0])
		}
		return lines, nil
	}

	return nil, fmt.Errorf("plain output requires explicit lines for multi-column data")
}
