package toon

import (
	"bytes"
	"strings"
	"testing"
)

func TestEncodeMirrorsUpstreamFixtureCategories(t *testing.T) {
	t.Parallel()

	type simpleObject struct {
		ID     int    `json:"id"`
		Name   string `json:"name"`
		Active bool   `json:"active"`
	}

	type nestedObject struct {
		User simpleObject `json:"user"`
	}

	type quotedKey struct {
		Value int `json:"order:id"`
	}

	type tabularRow struct {
		SKU   string  `json:"sku"`
		Qty   int     `json:"qty"`
		Price float64 `json:"price"`
	}

	type quotedHeaderRow struct {
		OrderID  int    `json:"order:id"`
		FullName string `json:"full name"`
	}

	type listRow struct {
		ID    int    `json:"id"`
		Name  string `json:"name"`
		Extra bool   `json:"extra"`
	}

	type arrayFirstRow struct {
		Nums []int  `json:"nums"`
		Name string `json:"name"`
	}

	type userRow struct {
		ID   int    `json:"id"`
		Name string `json:"name"`
	}

	type nestedArrayRow struct {
		Users  []userRow `json:"users"`
		Status string    `json:"status"`
	}

	testCases := []struct {
		name     string
		input    any
		expected string
	}{
		{
			name:     "fixture primitives safe string",
			input:    "Ada_99",
			expected: "Ada_99",
		},
		{
			name:     "fixture primitives quoted numeric-like string",
			input:    "42",
			expected: `"42"`,
		},
		{
			name:     "fixture primitives escaped newline",
			input:    "line1\nline2",
			expected: `"line1\nline2"`,
		},
		{
			name:     "fixture primitives quoted leading hyphen string",
			input:    map[string]any{"note": "- item"},
			expected: `note: "- item"`,
		},
		{
			name:     "fixture objects preserve field order",
			input:    simpleObject{ID: 123, Name: "Ada", Active: true},
			expected: "id: 123\nname: Ada\nactive: true",
		},
		{
			name:     "fixture objects nested object",
			input:    nestedObject{User: simpleObject{ID: 1, Name: "Ada", Active: true}},
			expected: "user:\n  id: 1\n  name: Ada\n  active: true",
		},
		{
			name:     "fixture objects quoted key",
			input:    quotedKey{Value: 7},
			expected: `"order:id": 7`,
		},
		{
			name:     "fixture arrays primitive inline",
			input:    map[string]any{"tags": []string{"reading", "gaming"}},
			expected: "tags[2]: reading,gaming",
		},
		{
			name:     "fixture arrays primitive empty",
			input:    map[string]any{"items": []string{}},
			expected: "items[0]:",
		},
		{
			name:     "fixture arrays primitive quoted members",
			input:    map[string]any{"items": []string{"a", "", "b,c"}},
			expected: `items[3]: a,"","b,c"`,
		},
		{
			name:     "fixture arrays tabular uniform objects",
			input:    map[string]any{"items": []tabularRow{{SKU: "A1", Qty: 2, Price: 9.99}, {SKU: "B2", Qty: 1, Price: 14.5}}},
			expected: "items[2]{sku,qty,price}:\n  A1,2,9.99\n  B2,1,14.5",
		},
		{
			name:     "fixture arrays tabular quoted headers",
			input:    map[string]any{"items": []quotedHeaderRow{{OrderID: 1, FullName: "Ada"}, {OrderID: 2, FullName: "Bob"}}},
			expected: "items[2]{\"order:id\",\"full name\"}:\n  1,Ada\n  2,Bob",
		},
		{
			name:     "fixture arrays nested arrays",
			input:    map[string]any{"pairs": [][]string{{"a", "b"}, {"c", "d"}}},
			expected: "pairs[2]:\n  - [2]: a,b\n  - [2]: c,d",
		},
		{
			name:     "fixture arrays nested root primitive array",
			input:    []any{"x", "y", "true", true, 10},
			expected: `[5]: x,y,"true",true,10`,
		},
		{
			name: "fixture arrays nested root uniform table",
			input: []struct {
				ID int `json:"id"`
			}{{ID: 1}, {ID: 2}},
			expected: "[2]{id}:\n  1\n  2",
		},
		{
			name:     "fixture arrays nested mixed list format",
			input:    map[string]any{"items": []any{1, map[string]any{"a": 1}, "text"}},
			expected: "items[3]:\n  - 1\n  - a: 1\n  - text",
		},
		{
			name:     "fixture arrays objects non-uniform rows",
			input:    map[string]any{"items": []any{simpleObject{ID: 1, Name: "First"}, listRow{ID: 2, Name: "Second", Extra: true}}},
			expected: "items[2]:\n  - id: 1\n    name: First\n    active: false\n  - id: 2\n    name: Second\n    extra: true",
		},
		{
			name:     "fixture arrays objects array first field canonical encoding",
			input:    map[string]any{"items": []arrayFirstRow{{Nums: []int{1, 2, 3}, Name: "Ada"}}},
			expected: "items[1]:\n  - nums[3]: 1,2,3\n    name: Ada",
		},
		{
			name:     "fixture arrays objects nested uniform table first field",
			input:    map[string]any{"items": []nestedArrayRow{{Users: []userRow{{ID: 1, Name: "Ada"}, {ID: 2, Name: "Bob"}}, Status: "active"}}},
			expected: "items[1]:\n  - users[2]{id,name}:\n      1,Ada\n      2,Bob\n    status: active",
		},
		{
			name:     "fixture whitespace no trailing newline",
			input:    map[string]any{"id": 123},
			expected: "id: 123",
		},
	}

	for _, testCase := range testCases {
		testCase := testCase
		t.Run(testCase.name, func(t *testing.T) {
			t.Parallel()

			var buffer bytes.Buffer
			if err := Encode(&buffer, testCase.input); err != nil {
				t.Fatalf("Encode() error = %v", err)
			}

			if got := buffer.String(); got != testCase.expected {
				t.Fatalf("Encode() = %q, want %q", got, testCase.expected)
			}

			if strings.HasSuffix(buffer.String(), "\n") {
				t.Fatalf("Encode() output should not end with newline: %q", buffer.String())
			}
		})
	}
}

func TestEncodeRejectsNilWriter(t *testing.T) {
	t.Parallel()

	err := Encode(nil, "value")
	if err == nil {
		t.Fatal("Encode(nil, ...) succeeded; want error")
	}
	if !strings.Contains(err.Error(), "writer") {
		t.Fatalf("Encode(nil, ...) error = %v", err)
	}
}
