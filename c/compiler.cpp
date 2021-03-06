#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =
  PREC_OR,          // or
  PREC_AND,         // and
  PREC_EQUALITY,    // == !=
  PREC_COMPARISON,  // < > <= >=
  PREC_TERM,        // + -
  PREC_FACTOR,      // * /
  PREC_UNARY,       // ! -
  PREC_CALL,        // . ()
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  // The name of the local variable.
  Token name;

  // The depth in the scope chain that this variable was declared at.
  // Zero is the outermost scope--parameters for a method, or the first
  // local block in top level code. One is the scope within that, etc.
  int depth;

  // True if this local variable is captured as an upvalue by a
  // function.
  bool isCaptured;
} Local;

typedef struct {
  // The index of the local variable or upvalue being captured from the
  // enclosing function.
  uint8_t index;

  // Whether the captured variable is a local or upvalue in the
  // enclosing function.
  bool isLocal;
} Upvalue;

typedef enum {
  TYPE_FUNCTION,
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_SCRIPT
} FunctionType;

typedef struct Compiler {
  // The compiler for the enclosing function, if any.
  struct Compiler* enclosing;

  // The function being compiled.
  OID<ObjFunction> function;
  FunctionType type;

  // The currently in scope local variables.
  Local locals[UINT8_COUNT];

  // The number of local variables currently in scope.
  int localCount;
  Upvalue upvalues[UINT8_COUNT];

  // The current level of block scope nesting. Zero is the outermost
  // local scope. 0 is global scope.
  int scopeDepth;
} Compiler;

typedef struct ClassCompiler {
  struct ClassCompiler* enclosing;

  Token name;
  bool hasSuperclass;
} ClassCompiler;

Parser parser;

Compiler* current = NULL;

ClassCompiler* currentClass = NULL;

static OID<ObjFunction> currentFunction() {
  return current->function;
}

static Chunk* currentChunk() {
  return &current->function.mlip().mp()->chunk;  //cb-resize-safe (no allocations in lifetimes of users)
}

static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) return;
  parser.panicMode = true;

  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static void error(const char* message) {
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

static void advance() {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser.current.start);
  }
}

static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

static bool check(TokenType type) {
  return parser.current.type == type;
}

static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte) {
  writeChunk(currentFunction().id(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

// Emits [instruction] followed by a placeholder for a jump offset. The
// placeholder can be patched by calling [jumpPatch]. Returns the index
// of the placeholder.
static int emitJump(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

static void emitReturn() {
  // An initializer automatically returns "this".
  if (current->type == TYPE_INITIALIZER) {
    emitBytes(OP_GET_LOCAL, 0);
  } else {
    emitByte(OP_NIL);
  }

  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentFunction().id(), value);
  if (constant > UINT8_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

static void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

// Replaces the placeholder argument for a previous CODE_JUMP or
// CODE_JUMP_IF instruction with an offset that jumps to the current
// end of bytecode.
static void patchJump(int offset) {
  // -2 to adjust for the bytecode for the jump offset itself.
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  currentChunk()->code.mlp().mp()[offset] = (jump >> 8) & 0xff;
  currentChunk()->code.mlp().mp()[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler* compiler, FunctionType type) {
  compiler->enclosing = current;
  compiler->function = CB_NULL_OID;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function = newFunction();
  current = compiler;

  if (type != TYPE_SCRIPT) {
    OID<ObjString> copiedString = copyString(parser.previous.start, parser.previous.length);
    current->function.mlip().mp()->name = copiedString;
  }

  // The first slot is always implicitly declared.
  Local* local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;
  if (type != TYPE_FUNCTION) {
    // In a method, it holds the receiver, "this".
    local->name.start = "this";
    local->name.length = 4;
  } else {
    // In a function, it holds the function, but cannot be referenced,
    // so has no name.
    local->name.start = "";
    local->name.length = 0;
  }
}

static OID<ObjFunction> endCompiler() {
  emitReturn();

  OID<ObjFunction> function = current->function;
#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(),
        function.clip().cp()->name.is_nil() ? "<top>" : function.clip().cp()->name.clip().cp()->chars.clp().cp());
  }
#endif
  current = current->enclosing;

  return function;
}

static void beginScope() {
  current->scopeDepth++;
}

static void endScope() {
  current->scopeDepth--;

  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth >
            current->scopeDepth)
  {
    if (current->locals[current->localCount - 1].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
    current->localCount--;
  }
}

static void expression();
static void statement();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

static uint8_t identifierConstant(Token* name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length).id()));
}

static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
  // Look it up in the local scopes. Look in reverse order so that the
  // most nested variable is found first and shadows outer ones.
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        error("Cannot read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1;
}

// Adds an upvalue to [compiler]'s function with the given properties.
// Does not add one if an upvalue for that variable is already in the
// list. Returns the index of the upvalue.
static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
  // Look for an existing one.
  int upvalueCount = compiler->function.clip().cp()->upvalueCount;
  for (int i = 0; i < upvalueCount; i++) {
    Upvalue* upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  // If we got here, it's a new upvalue.
  if (upvalueCount == UINT8_COUNT) {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function.mlip().mp()->upvalueCount++;
}

// Attempts to look up [name] in the functions enclosing the one being
// compiled by [compiler]. If found, it adds an upvalue for it to this
// compiler's list of upvalues (unless it's already in there) and
// returns its index. If not found, returns -1.
//
// If the name is found outside of the immediately enclosing function,
// this will flatten the closure and add upvalues to all of the
// intermediate functions so that it gets walked down to this one.
static int resolveUpvalue(Compiler* compiler, Token* name) {
  // If we are at the top level, we didn't find it.
  if (compiler->enclosing == NULL) return -1;

  // See if it's a local variable in the immediately enclosing function.
  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    // Mark the local as an upvalue so we know to close it when it goes
    // out of scope.
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  // See if it's an upvalue in the immediately enclosing function. In
  // other words, if it's a local variable in a non-immediately
  // enclosing function. This "flattens" closures automatically: it
  // adds upvalues to all of the intermediate functions to get from the
  // function where a local is declared all the way into the possibly
  // deeply nested function that is closing over it.
  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }

  // If we got here, we walked all the way up the parent chain and
  // couldn't find it.
  return -1;
}

static void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function.");
    return;
  }

  Local* local = &current->locals[current->localCount];
  local->name = name;

  // The local is declared but not yet defined.
  local->depth = -1;
  local->isCaptured = false;
  current->localCount++;
}

// Allocates a local slot for the value currently on the stack, if
// we're in a local scope.
static void declareVariable() {
  // Global variables are implicitly declared.
  if (current->scopeDepth == 0) return;

  // See if a local variable with this name is already declared in this
  // scope.
  Token* name = &parser.previous;
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local* local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth) break;
    if (identifiersEqual(name, &local->name)) {
      error("Variable with this name already declared in this scope.");
    }
  }

  addLocal(*name);
}

static uint8_t parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable();
  if (current->scopeDepth > 0) return 0;

  return identifierConstant(&parser.previous);
}

static void markInitialized() {
  if (current->scopeDepth == 0) return;
  current->locals[current->localCount - 1].depth =
      current->scopeDepth;
}

static void defineVariable(uint8_t global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList() {
  uint8_t argCount = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();

      if (argCount == 255) {
        error("Cannot have more than 255 arguments.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  return argCount;
}

static void and_(bool canAssign) {
  // left operand...
  // OP_JUMP_IF       ------.
  // OP_POP // left operand |
  // right operand...       |
  //   <--------------------'
  // ...

  // Short circuit if the left operand is false.
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  // Compile the right operand.
  emitByte(OP_POP); // Left operand.
  parsePrecedence(PREC_AND);

  patchJump(endJump);
}

static void binary(bool canAssign) {
  // Remember the operator.
  TokenType operatorType = parser.previous.type;

  // Compile the right operand.
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  // Emit the operator instruction.
  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_GREATER); break;
    case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_MINUS:         emitByte(OP_SUBTRACT); break;
    case TOKEN_STAR:          emitByte(OP_MULTIPLY); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    default:
      return; // Unreachable.
  }
}

static void call(bool canAssign) {
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void dot(bool canAssign) {
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
  uint8_t name = identifierConstant(&parser.previous);

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_SET_PROPERTY, name);
  } else if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCount = argumentList();
    emitBytes(OP_INVOKE, name);
    emitByte(argCount);
  } else {
    emitBytes(OP_GET_PROPERTY, name);
  }
}

static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default:
      return; // Unreachable.
  }
}

static void grouping(bool canAssign) {
//< Global Variables grouping
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
  // left operand...
  // OP_JUMP_IF       ---.
  // OP_JUMP          ---+--.
  //   <-----------------'  |
  // OP_POP // left operand |
  // right operand...       |
  //   <--------------------'
  // ...

  // If the operand is *true* we want to keep it, so when it's false,
  // jump to the code to evaluate the right operand.
  int elseJump = emitJump(OP_JUMP_IF_FALSE);

  // If we get here, the operand is true, so jump to the end to keep it.
  int endJump = emitJump(OP_JUMP);

  // Compile the right operand.
  patchJump(elseJump);
  emitByte(OP_POP); // Left operand.

  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void string(bool canAssign) {
  emitConstant(OBJ_VAL(copyString(parser.previous.start + 1,
                                  parser.previous.length - 2).id()));
}

static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name);
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (uint8_t)arg);
  } else {
    emitBytes(getOp, (uint8_t)arg);
  }
}

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static Token syntheticToken(const char* text) {
  Token token;
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

static void super_(bool canAssign) {
  if (currentClass == NULL) {
    error("Cannot use 'super' outside of a class.");
  } else if (!currentClass->hasSuperclass) {
    error("Cannot use 'super' in a class with no superclass.");
  }

  consume(TOKEN_DOT, "Expect '.' after 'super'.");
  consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
  uint8_t name = identifierConstant(&parser.previous);

  // Push the receiver.
  namedVariable(syntheticToken("this"), false);

  if (match(TOKEN_LEFT_PAREN)) {
    uint8_t argCount = argumentList();

    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_SUPER_INVOKE, name);
    emitByte(argCount);
  } else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_GET_SUPER, name);
  }
}

static void this_(bool canAssign) {
  if (currentClass == NULL) {
    error("Cannot use 'this' outside of a class.");
    return;
  }
  variable(false);
}

static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;

  // Compile the operand.
  parsePrecedence(PREC_UNARY);

  // Emit the operator instruction.
  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
    case TOKEN_MINUS: emitByte(OP_NEGATE); break;
    default:
      return; // Unreachable.
  }
}

ParseRule rules[] = {
  { grouping, call,    PREC_CALL },       // TOKEN_LEFT_PAREN
  { NULL,     NULL,    PREC_NONE },       // TOKEN_RIGHT_PAREN
  { NULL,     NULL,    PREC_NONE },       // TOKEN_LEFT_BRACE [big]
  { NULL,     NULL,    PREC_NONE },       // TOKEN_RIGHT_BRACE
  { NULL,     NULL,    PREC_NONE },       // TOKEN_COMMA
  { NULL,     dot,     PREC_CALL },       // TOKEN_DOT
  { unary,    binary,  PREC_TERM },       // TOKEN_MINUS
  { NULL,     binary,  PREC_TERM },       // TOKEN_PLUS
  { NULL,     NULL,    PREC_NONE },       // TOKEN_SEMICOLON
  { NULL,     binary,  PREC_FACTOR },     // TOKEN_SLASH
  { NULL,     binary,  PREC_FACTOR },     // TOKEN_STAR
  { unary,    NULL,    PREC_NONE },       // TOKEN_BANG
  { NULL,     binary,  PREC_EQUALITY },   // TOKEN_BANG_EQUAL
  { NULL,     NULL,    PREC_NONE },       // TOKEN_EQUAL
  { NULL,     binary,  PREC_EQUALITY },   // TOKEN_EQUAL_EQUAL
  { NULL,     binary,  PREC_COMPARISON }, // TOKEN_GREATER
  { NULL,     binary,  PREC_COMPARISON }, // TOKEN_GREATER_EQUAL
  { NULL,     binary,  PREC_COMPARISON }, // TOKEN_LESS
  { NULL,     binary,  PREC_COMPARISON }, // TOKEN_LESS_EQUAL
  { variable, NULL,    PREC_NONE },       // TOKEN_IDENTIFIER
  { string,   NULL,    PREC_NONE },       // TOKEN_STRING
  { number,   NULL,    PREC_NONE },       // TOKEN_NUMBER
  { NULL,     and_,    PREC_AND },        // TOKEN_AND
  { NULL,     NULL,    PREC_NONE },       // TOKEN_CLASS
  { NULL,     NULL,    PREC_NONE },       // TOKEN_ELSE
  { literal,  NULL,    PREC_NONE },       // TOKEN_FALSE
  { NULL,     NULL,    PREC_NONE },       // TOKEN_FOR
  { NULL,     NULL,    PREC_NONE },       // TOKEN_FUN
  { NULL,     NULL,    PREC_NONE },       // TOKEN_IF
  { literal,  NULL,    PREC_NONE },       // TOKEN_NIL
  { NULL,     or_,     PREC_OR },         // TOKEN_OR
  { NULL,     NULL,    PREC_NONE },       // TOKEN_PRINT
  { NULL,     NULL,    PREC_NONE },       // TOKEN_RETURN
  { super_,   NULL,    PREC_NONE },       // TOKEN_SUPER
  { this_,    NULL,    PREC_NONE },       // TOKEN_THIS
  { literal,  NULL,    PREC_NONE },       // TOKEN_TRUE
  { NULL,     NULL,    PREC_NONE },       // TOKEN_VAR
  { NULL,     NULL,    PREC_NONE },       // TOKEN_WHILE
  { NULL,     NULL,    PREC_NONE },       // TOKEN_ERROR
  { NULL,     NULL,    PREC_NONE },       // TOKEN_EOF
};

static void parsePrecedence(Precedence precedence) {
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);

  while (precedence <= getRule(parser.current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    infixRule(canAssign);
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    error("Invalid assignment target.");
  }
}

static ParseRule* getRule(TokenType type) {
  return &rules[type];
}

static void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope(); // [no-end-scope]

  // Compile the parameter list.
  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");

  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->function.mlip().mp()->arity++;
      if (current->function.clip().cp()->arity > 255) {
        errorAtCurrent("Cannot have more than 255 parameters.");
      }

      uint8_t paramConstant = parseVariable("Expect parameter name.");
      defineVariable(paramConstant);
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

  // The body.
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block();

  // Create the function object.
  OID<ObjFunction> function = endCompiler();

  // Capture the upvalues in the new closure object.
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function.id())));

  // Emit arguments for each upvalue to know whether to capture a local
  // or an upvalue.
  for (int i = 0, e = function.clip().cp()->upvalueCount; i < e; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void method() {
  consume(TOKEN_IDENTIFIER, "Expect method name.");
  uint8_t constant = identifierConstant(&parser.previous);

  // If the method is named "init", it's an initializer.
  FunctionType type = TYPE_METHOD;
  if (parser.previous.length == 4 &&
      memcmp(parser.previous.start, "init", 4) == 0) {
    type = TYPE_INITIALIZER;
  }

  function(type);

  emitBytes(OP_METHOD, constant);
}

static void classDeclaration() {
  consume(TOKEN_IDENTIFIER, "Expect class name.");
  Token className = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();

  emitBytes(OP_CLASS, nameConstant);
  defineVariable(nameConstant);

  ClassCompiler classCompiler;
  classCompiler.name = parser.previous;
  classCompiler.hasSuperclass = false;
  classCompiler.enclosing = currentClass;
  currentClass = &classCompiler;

  if (match(TOKEN_LESS)) {
    consume(TOKEN_IDENTIFIER, "Expect superclass name.");
    variable(false);

    if (identifiersEqual(&className, &parser.previous)) {
      error("A class cannot inherit from itself.");
    }

    // Store the superclass in a local variable named "super".
    beginScope();
    addLocal(syntheticToken("super"));
    defineVariable(0);

    namedVariable(className, false);
    emitByte(OP_INHERIT);
    classCompiler.hasSuperclass = true;
  }

  namedVariable(className, false);
  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    method();
  }
  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
  emitByte(OP_POP);

  if (classCompiler.hasSuperclass) {
    endScope();
  }

  currentClass = currentClass->enclosing;
}

static void funDeclaration() {
  uint8_t global = parseVariable("Expect function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

static void varDeclaration() {
  uint8_t global = parseVariable("Expect variable name.");

  if (match(TOKEN_EQUAL)) {
    expression();
  } else {
    emitByte(OP_NIL);
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  defineVariable(global);
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void forStatement() {
  // for (var i = 0; i < 10; i = i + 1) print i;
  //
  //   var i = 0;
  // start:                      <--.
  //   if (i < 10) goto exit;  --.  |
  //   goto body;  -----------.  |  |
  // increment:            <--+--+--+--.
  //   i = i + 1;             |  |  |  |
  //   goto start;  ----------+--+--'  |
  // body:                 <--'  |     |
  //   print i;                  |     |
  //   goto increment;  ---------+-----'
  // exit:                    <--'

  // Create a scope for the loop variable.
  beginScope();

  // The initialization clause.
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
  } else {
    expressionStatement();
  }

  int loopStart = currentChunk()->count;

  // The exit condition.
  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }

  // Increment step.
  if (!match(TOKEN_RIGHT_PAREN)) {
    // We don't want to execute the increment before the body, so jump
    // over it.
    int bodyJump = emitJump(OP_JUMP);

    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    // After the increment, start the whole loop over.
    emitLoop(loopStart);

    // At the end of the body, we want to jump to the increment, not
    // the top of the loop.
    loopStart = incrementStart;

    patchJump(bodyJump);
  }

  // Compile the body.
  statement();

  // Jump back to the beginning (or the increment).
  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  endScope(); // Loop variable.
}

static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition."); // [paren]

  // Compile the then branch.
  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  // Jump over the else branch when the if branch is taken.
  int elseJump = emitJump(OP_JUMP);

  // Compile the else branch.
  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();

  patchJump(elseJump);
}

static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  emitByte(OP_PRINT);
}

static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Cannot return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  } else {
    if (current->type == TYPE_INITIALIZER) {
      error("Cannot return a value from an initializer.");
    }

    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void whileStatement() {
  int loopStart = currentChunk()->count;

  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  // Jump out of the loop if the condition is false.
  int exitJump = emitJump(OP_JUMP_IF_FALSE);

  // Compile the body.
  emitByte(OP_POP); // Condition.
  statement();

  // Loop back to the start.
  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP); // Condition.
}

static void synchronize() {
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;

    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        return;

      default:
        // Do nothing.
        ;
    }

    advance();
  }
}

static void declaration() {
  if (match(TOKEN_CLASS)) {
    classDeclaration();
  } else if (match(TOKEN_FUN)) {
    funDeclaration();
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}

static void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_FOR)) {
    forStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_RETURN)) {
    returnStatement();
  } else if (match(TOKEN_WHILE)) {
    whileStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

OID<ObjFunction> compile(const char* source) {
  initScanner(source);
  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);

  parser.hadError = false;
  parser.panicMode = false;

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  OID<ObjFunction> function = endCompiler();

  // If there was a compile error, the code is not valid, so don't
  // create a function.
  return parser.hadError ? OID<ObjFunction>(CB_NULL_OID) : function;
}

void grayCompilerRoots() {
  Compiler* compiler = current;
  while (compiler != NULL) {
    grayObject(compiler->function.id());
    compiler = compiler->enclosing;
  }
}
