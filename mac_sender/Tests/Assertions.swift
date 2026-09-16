import Testing

func XCTAssertEqual<T: Equatable>(_ actual: T, _ expected: T) { #expect(actual == expected) }
func XCTAssertTrue(_ value: Bool) { #expect(value) }
func XCTAssertNil<T>(_ value: T?) { #expect(value == nil) }
func XCTAssertThrowsError<T>(_ expression: @autoclosure () throws -> T, _ message: String = "") {
    #expect(throws: (any Error).self) { _ = try expression() }
}
func XCTAssertNoThrow<T>(_ expression: @autoclosure () throws -> T) {
    #expect(throws: Never.self) { _ = try expression() }
}
