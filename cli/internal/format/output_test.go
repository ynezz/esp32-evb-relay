package format

import (
	"bytes"
	"strings"
	"testing"
)

func TestResolve(t *testing.T) {
	t.Parallel()

	testCases := []struct {
		name      string
		requested string
		robot     bool
		want      string
		wantErr   string
	}{
		{name: "human default", requested: "", robot: false, want: Table},
		{name: "human json", requested: " JSON ", robot: false, want: JSON},
		{name: "robot default", requested: "", robot: true, want: TOON},
		{name: "robot json override", requested: "json", robot: true, want: JSON},
		{name: "robot rejects table", requested: "table", robot: true, wantErr: "invalid robot format"},
		{name: "human rejects toon", requested: "toon", robot: false, wantErr: "unsupported format"},
	}

	for _, testCase := range testCases {
		testCase := testCase
		t.Run(testCase.name, func(t *testing.T) {
			t.Parallel()

			got, err := Resolve(testCase.requested, testCase.robot)
			if testCase.wantErr != "" {
				if err == nil {
					t.Fatalf("Resolve(%q, %t) succeeded; want error containing %q", testCase.requested, testCase.robot, testCase.wantErr)
				}
				if !strings.Contains(err.Error(), testCase.wantErr) {
					t.Fatalf("Resolve(%q, %t) error = %q; want substring %q", testCase.requested, testCase.robot, err, testCase.wantErr)
				}
				return
			}

			if err != nil {
				t.Fatalf("Resolve(%q, %t) error = %v", testCase.requested, testCase.robot, err)
			}
			if got != testCase.want {
				t.Fatalf("Resolve(%q, %t) = %q; want %q", testCase.requested, testCase.robot, got, testCase.want)
			}
		})
	}
}

func TestOutputJSON(t *testing.T) {
	t.Parallel()

	var buffer bytes.Buffer
	data := map[string]any{
		"hostname": "esp32-evb-relay",
		"port":     80,
	}

	if err := Output(&buffer, data, JSON); err != nil {
		t.Fatalf("Output(JSON) error = %v", err)
	}

	got := buffer.String()
	if !strings.Contains(got, "\"hostname\": \"esp32-evb-relay\"") {
		t.Fatalf("JSON output missing hostname field: %q", got)
	}
	if !strings.HasSuffix(got, "\n") {
		t.Fatalf("JSON output should end with newline: %q", got)
	}
}

func TestOutputTable(t *testing.T) {
	t.Parallel()

	var buffer bytes.Buffer
	data := HumanData{
		Table: TableData{
			Headers: []string{"group", "id", "state"},
			Rows: [][]string{
				{"onboard", "1", "true"},
				{"modio", "3", "unknown"},
			},
		},
	}

	if err := Output(&buffer, data, Table); err != nil {
		t.Fatalf("Output(Table) error = %v", err)
	}

	lines := strings.Split(strings.TrimSpace(buffer.String()), "\n")
	if got := strings.Fields(lines[0]); !equalStrings(got, []string{"group", "id", "state"}) {
		t.Fatalf("table header fields = %v", got)
	}
	if got := strings.Fields(lines[1]); !equalStrings(got, []string{"onboard", "1", "true"}) {
		t.Fatalf("table row 1 fields = %v", got)
	}
	if got := strings.Fields(lines[2]); !equalStrings(got, []string{"modio", "3", "unknown"}) {
		t.Fatalf("table row 2 fields = %v", got)
	}
}

func TestOutputPlainExplicitLines(t *testing.T) {
	t.Parallel()

	var buffer bytes.Buffer
	data := HumanData{
		Table: TableData{
			Headers: []string{"group", "id"},
			Rows: [][]string{
				{"onboard", "1"},
			},
		},
		Plain: []string{"onboard:1"},
	}

	if err := Output(&buffer, data, Plain); err != nil {
		t.Fatalf("Output(Plain) error = %v", err)
	}

	if got := buffer.String(); got != "onboard:1\n" {
		t.Fatalf("plain output = %q", got)
	}
}

func TestOutputPlainSingleColumnFallback(t *testing.T) {
	t.Parallel()

	var buffer bytes.Buffer
	data := HumanData{
		Table: TableData{
			Headers: []string{"target"},
			Rows: [][]string{
				{"onboard:1"},
				{"modio:4"},
			},
		},
	}

	if err := Output(&buffer, data, Plain); err != nil {
		t.Fatalf("Output(Plain) error = %v", err)
	}

	if got := buffer.String(); got != "onboard:1\nmodio:4\n" {
		t.Fatalf("plain output = %q", got)
	}
}

func TestOutputPlainRejectsImplicitMultiColumn(t *testing.T) {
	t.Parallel()

	data := HumanData{
		Table: TableData{
			Headers: []string{"group", "id"},
			Rows: [][]string{
				{"onboard", "1"},
			},
		},
	}

	err := Output(&bytes.Buffer{}, data, Plain)
	if err == nil {
		t.Fatal("Output(Plain) succeeded; want error")
	}
	if !strings.Contains(err.Error(), "plain output requires explicit lines") {
		t.Fatalf("Output(Plain) error = %v", err)
	}
}

func TestOutputTableRejectsInvalidRows(t *testing.T) {
	t.Parallel()

	data := TableData{
		Headers: []string{"group", "id"},
		Rows: [][]string{
			{"onboard"},
		},
	}

	err := Output(&bytes.Buffer{}, data, Table)
	if err == nil {
		t.Fatal("Output(Table) succeeded; want error")
	}
	if !strings.Contains(err.Error(), "expected 2") {
		t.Fatalf("Output(Table) error = %v", err)
	}
}

func equalStrings(left []string, right []string) bool {
	if len(left) != len(right) {
		return false
	}

	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}

	return true
}
