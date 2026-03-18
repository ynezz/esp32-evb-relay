package toon

import (
	"encoding"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"reflect"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const indentUnit = "  "

var numericLikePattern = regexp.MustCompile(`^[+-]?(?:\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)$`)

type field struct {
	name  string
	value any
}

type jsonTag struct {
	name      string
	omitEmpty bool
}

func Encode(w io.Writer, v any) error {
	if w == nil {
		return errors.New("toon output writer is required")
	}

	lines, err := encodeValue(v)
	if err != nil {
		return err
	}

	_, err = io.WriteString(w, strings.Join(lines, "\n"))
	return err
}

func encodeValue(v any) ([]string, error) {
	switch {
	case isPrimitive(v):
		return []string{encodeScalar(v)}, nil
	case isArray(v):
		return encodeArray("", v)
	case isObject(v):
		return encodeObject(v)
	default:
		return nil, fmt.Errorf("toon: unsupported value type %T", v)
	}
}

func encodeObject(v any) ([]string, error) {
	fields, err := objectFields(v)
	if err != nil {
		return nil, err
	}

	lines := make([]string, 0, len(fields))
	for _, current := range fields {
		fieldLines, err := encodeField(current.name, current.value)
		if err != nil {
			return nil, err
		}
		lines = append(lines, fieldLines...)
	}

	return lines, nil
}

func encodeField(name string, value any) ([]string, error) {
	switch {
	case isPrimitive(value):
		return []string{encodeKey(name) + ": " + encodeScalar(value)}, nil
	case isArray(value):
		return encodeArray(name, value)
	case isObject(value):
		childLines, err := encodeObject(value)
		if err != nil {
			return nil, err
		}
		lines := []string{encodeKey(name) + ":"}
		lines = append(lines, prefixLines(childLines, indentUnit)...)
		return lines, nil
	default:
		return nil, fmt.Errorf("toon: unsupported field %q type %T", name, value)
	}
}

func encodeArray(name string, value any) ([]string, error) {
	elements, err := arrayElements(value)
	if err != nil {
		return nil, err
	}

	headerPrefix := encodeKey(name)
	if name == "" {
		headerPrefix = ""
	}

	if isPrimitiveArray(elements) {
		header := encodeArrayHeader(headerPrefix, len(elements), nil)
		if len(elements) == 0 {
			return []string{header}, nil
		}

		values := make([]string, 0, len(elements))
		for _, element := range elements {
			values = append(values, encodeScalar(element))
		}

		return []string{header + " " + strings.Join(values, ",")}, nil
	}

	fields, ok, err := tabularFields(elements)
	if err != nil {
		return nil, err
	}
	if ok {
		lines := []string{encodeArrayHeader(headerPrefix, len(elements), fields)}
		for _, element := range elements {
			row, rowErr := encodeTabularRow(element, fields)
			if rowErr != nil {
				return nil, rowErr
			}
			lines = append(lines, indentUnit+row)
		}
		return lines, nil
	}

	lines := []string{encodeArrayHeader(headerPrefix, len(elements), nil)}
	for _, element := range elements {
		itemLines, itemErr := encodeListItem(element)
		if itemErr != nil {
			return nil, itemErr
		}
		lines = append(lines, prefixLines(itemLines, indentUnit)...)
	}

	return lines, nil
}

func encodeListItem(value any) ([]string, error) {
	switch {
	case isPrimitive(value):
		return []string{"- " + encodeScalar(value)}, nil
	case isArray(value):
		lines, err := encodeArray("", value)
		if err != nil {
			return nil, err
		}
		return listify(lines), nil
	case isObject(value):
		fields, err := objectFields(value)
		if err != nil {
			return nil, err
		}
		if len(fields) == 0 {
			return []string{"- {}"}, nil
		}

		firstLines, err := encodeField(fields[0].name, fields[0].value)
		if err != nil {
			return nil, err
		}

		lines := listify(firstLines)
		for _, current := range fields[1:] {
			fieldLines, fieldErr := encodeField(current.name, current.value)
			if fieldErr != nil {
				return nil, fieldErr
			}
			lines = append(lines, prefixLines(fieldLines, indentUnit)...)
		}
		return lines, nil
	default:
		return nil, fmt.Errorf("toon: unsupported list item type %T", value)
	}
}

func listify(lines []string) []string {
	if len(lines) == 0 {
		return []string{"-"}
	}

	items := []string{"- " + lines[0]}
	if len(lines) > 1 {
		items = append(items, prefixLines(lines[1:], indentUnit)...)
	}
	return items
}

func prefixLines(lines []string, prefix string) []string {
	if len(lines) == 0 {
		return nil
	}

	prefixed := make([]string, 0, len(lines))
	for _, line := range lines {
		prefixed = append(prefixed, prefix+line)
	}
	return prefixed
}

func encodeTabularRow(value any, fields []string) (string, error) {
	rowFields, err := objectFields(value)
	if err != nil {
		return "", err
	}

	rowByName := make(map[string]any, len(rowFields))
	for _, current := range rowFields {
		rowByName[current.name] = current.value
	}

	values := make([]string, 0, len(fields))
	for _, name := range fields {
		values = append(values, encodeScalar(rowByName[name]))
	}

	return strings.Join(values, ","), nil
}

func encodeArrayHeader(name string, length int, fields []string) string {
	header := fmt.Sprintf("%s[%d]", name, length)
	if len(fields) > 0 {
		encodedFields := make([]string, 0, len(fields))
		for _, fieldName := range fields {
			encodedFields = append(encodedFields, encodeKey(fieldName))
		}
		header += "{" + strings.Join(encodedFields, ",") + "}"
	}

	return header + ":"
}

func tabularFields(elements []any) ([]string, bool, error) {
	if len(elements) == 0 {
		return nil, false, nil
	}

	fields, err := objectFields(elements[0])
	if err != nil {
		return nil, false, nil
	}
	if len(fields) == 0 {
		return nil, false, nil
	}

	shape := make([]string, 0, len(fields))
	for _, current := range fields {
		if !isPrimitive(current.value) {
			return nil, false, nil
		}
		shape = append(shape, current.name)
	}

	for _, element := range elements[1:] {
		currentFields, currentErr := objectFields(element)
		if currentErr != nil || len(currentFields) != len(shape) {
			return nil, false, nil
		}
		for index, current := range currentFields {
			if current.name != shape[index] || !isPrimitive(current.value) {
				return nil, false, nil
			}
		}
	}

	return shape, true, nil
}

func objectFields(v any) ([]field, error) {
	value := indirectValue(reflect.ValueOf(v))
	if !value.IsValid() {
		return nil, nil
	}

	switch value.Kind() {
	case reflect.Struct:
		result := make([]field, 0, value.NumField())
		valueType := value.Type()
		for index := 0; index < value.NumField(); index++ {
			structField := valueType.Field(index)
			if structField.PkgPath != "" {
				continue
			}
			tag := parseJSONTag(structField)
			if tag.name == "-" {
				continue
			}
			if tag.omitEmpty && isEmptyValue(value.Field(index)) {
				continue
			}
			result = append(result, field{
				name:  fieldName(structField, tag),
				value: value.Field(index).Interface(),
			})
		}
		return result, nil
	case reflect.Map:
		if value.Type().Key().Kind() != reflect.String {
			return nil, fmt.Errorf("toon: unsupported map key type %s", value.Type().Key())
		}

		keys := value.MapKeys()
		sort.Slice(keys, func(left, right int) bool {
			return keys[left].String() < keys[right].String()
		})

		result := make([]field, 0, len(keys))
		for _, key := range keys {
			result = append(result, field{
				name:  key.String(),
				value: value.MapIndex(key).Interface(),
			})
		}
		return result, nil
	default:
		return nil, fmt.Errorf("toon: unsupported object type %T", v)
	}
}

func parseJSONTag(structField reflect.StructField) jsonTag {
	tag := structField.Tag.Get("json")
	if tag == "" {
		return jsonTag{}
	}

	parts := strings.Split(tag, ",")
	parsed := jsonTag{name: parts[0]}
	for _, option := range parts[1:] {
		if option == "omitempty" {
			parsed.omitEmpty = true
		}
	}

	return parsed
}

func fieldName(structField reflect.StructField, tag jsonTag) string {
	if tag.name != "" {
		return tag.name
	}
	return structField.Name
}

func isEmptyValue(value reflect.Value) bool {
	switch value.Kind() {
	case reflect.Array, reflect.Map, reflect.Slice, reflect.String:
		return value.Len() == 0
	case reflect.Bool:
		return !value.Bool()
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return value.Int() == 0
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return value.Uint() == 0
	case reflect.Float32, reflect.Float64:
		return value.Float() == 0
	case reflect.Interface, reflect.Pointer:
		return value.IsNil()
	default:
		return value.IsZero()
	}
}

func arrayElements(v any) ([]any, error) {
	value := indirectValue(reflect.ValueOf(v))
	if !value.IsValid() {
		return nil, fmt.Errorf("toon: invalid array value")
	}
	if value.Kind() != reflect.Array && value.Kind() != reflect.Slice {
		return nil, fmt.Errorf("toon: unsupported array type %T", v)
	}

	elements := make([]any, 0, value.Len())
	for index := 0; index < value.Len(); index++ {
		elements = append(elements, value.Index(index).Interface())
	}
	return elements, nil
}

func isPrimitiveArray(elements []any) bool {
	for _, element := range elements {
		if !isPrimitive(element) {
			return false
		}
	}
	return true
}

func isPrimitive(v any) bool {
	value := indirectValue(reflect.ValueOf(v))
	if !value.IsValid() {
		return true
	}

	if _, ok := v.(json.Number); ok {
		return true
	}
	if _, ok := v.(encoding.TextMarshaler); ok {
		return true
	}

	switch value.Kind() {
	case reflect.Bool, reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32,
		reflect.Int64, reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32,
		reflect.Uint64, reflect.Uintptr, reflect.Float32, reflect.Float64, reflect.String:
		return true
	default:
		return false
	}
}

func isObject(v any) bool {
	value := indirectValue(reflect.ValueOf(v))
	return value.IsValid() && (value.Kind() == reflect.Map || value.Kind() == reflect.Struct)
}

func isArray(v any) bool {
	value := indirectValue(reflect.ValueOf(v))
	return value.IsValid() && (value.Kind() == reflect.Array || value.Kind() == reflect.Slice)
}

func indirectValue(value reflect.Value) reflect.Value {
	for value.IsValid() && (value.Kind() == reflect.Interface || value.Kind() == reflect.Pointer) {
		if value.IsNil() {
			return reflect.Value{}
		}
		value = value.Elem()
	}
	return value
}

func encodeScalar(v any) string {
	if marshaler, ok := v.(encoding.TextMarshaler); ok {
		text, err := marshaler.MarshalText()
		if err == nil {
			return encodeString(string(text), false)
		}
	}

	if number, ok := v.(json.Number); ok {
		if integer, err := number.Int64(); err == nil {
			return strconv.FormatInt(integer, 10)
		}
		if floating, err := number.Float64(); err == nil {
			return formatFloat(floating, 64)
		}
		return encodeString(number.String(), false)
	}

	value := indirectValue(reflect.ValueOf(v))
	if !value.IsValid() {
		return "null"
	}

	switch value.Kind() {
	case reflect.String:
		return encodeString(value.String(), false)
	case reflect.Bool:
		return strconv.FormatBool(value.Bool())
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return strconv.FormatInt(value.Int(), 10)
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return strconv.FormatUint(value.Uint(), 10)
	case reflect.Float32:
		return formatFloat(value.Float(), 32)
	case reflect.Float64:
		return formatFloat(value.Float(), 64)
	default:
		return encodeString(fmt.Sprint(v), false)
	}
}

func formatFloat(value float64, bits int) string {
	if math.IsNaN(value) || math.IsInf(value, 0) {
		return "null"
	}
	return strconv.FormatFloat(value, 'f', -1, bits)
}

func encodeKey(key string) string {
	return encodeString(key, true)
}

func encodeString(value string, key bool) string {
	if needsQuotes(value, key) {
		return `"` + escapeString(value) + `"`
	}
	return value
}

func needsQuotes(value string, key bool) bool {
	if value == "" || value != strings.TrimSpace(value) {
		return true
	}
	if value == "true" || value == "false" || value == "null" || numericLikePattern.MatchString(value) {
		return true
	}
	if strings.HasPrefix(value, "-") {
		return true
	}

	for _, r := range value {
		if key && r == ' ' {
			return true
		}
		switch r {
		case ',', ':', '"', '\\', '[', ']', '{', '}', '\n', '\r', '\t':
			return true
		}
	}

	return false
}

func escapeString(value string) string {
	var builder strings.Builder
	builder.Grow(len(value))

	for _, r := range value {
		switch r {
		case '\\':
			builder.WriteString(`\\`)
		case '"':
			builder.WriteString(`\"`)
		case '\n':
			builder.WriteString(`\n`)
		case '\r':
			builder.WriteString(`\r`)
		case '\t':
			builder.WriteString(`\t`)
		default:
			builder.WriteRune(r)
		}
	}

	return builder.String()
}
