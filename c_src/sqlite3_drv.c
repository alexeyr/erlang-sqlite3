#include "sqlite3_drv.h"

// MSVC needs "__inline" instead of "inline" in C-source files.
#if defined(_MSC_VER)
#  define inline __inline
#endif

static ErlDrvEntry basic_driver_entry = {
    NULL, /* init */
    start, /* startup (defined below) */
    stop, /* shutdown (defined below) */
    NULL, /* output */
    NULL, /* ready_input */
    NULL, /* ready_output */
    "sqlite3_drv", /* the name of the driver */
    NULL, /* finish */
    NULL, /* handle */
    control, /* control */
    NULL, /* timeout */
    NULL, /* outputv */
    ready_async, /* ready_async (defined below) */
    NULL, /* flush */
    NULL, /* call */
    NULL, /* event */
    ERL_DRV_EXTENDED_MARKER, /* ERL_DRV_EXTENDED_MARKER */
    ERL_DRV_EXTENDED_MAJOR_VERSION, /* ERL_DRV_EXTENDED_MAJOR_VERSION */
    ERL_DRV_EXTENDED_MAJOR_VERSION, /* ERL_DRV_EXTENDED_MINOR_VERSION */
    ERL_DRV_FLAG_USE_PORT_LOCKING, /* ERL_DRV_FLAGs */
    NULL /* handle2 */,
    NULL /* process_exit */,
    NULL /* stop_select */
};

DRIVER_INIT(basic_driver) {
  return &basic_driver_entry;
}

static inline ptr_list *add_to_ptr_list(ptr_list *list, void *value_ptr);
static inline void free_ptr_list(ptr_list *list, void(* free_head)(void *));
static inline int max(int a, int b);
static inline int sql_is_insert(const char *sql);

// Driver Start
static ErlDrvData start(ErlDrvPort port, char* cmd) {
  sqlite3_drv_t* retval = (sqlite3_drv_t*) driver_alloc(sizeof(sqlite3_drv_t));
  struct sqlite3 *db = NULL;
  int status = 0;

  retval->log = fopen(LOG_PATH, "a+");
  if (!retval->log) {
    fprintf(stderr, "Can't create log file\n");
  }

  fprintf(retval->log,
          "--- Start erlang-sqlite3 driver\nCommand line: [%s]\n", cmd);

  const char *db_name = strstr(cmd, " ");
  if (!db_name) {
    fprintf(retval->log,
            "ERROR: DB name should be passed at command line\n");
    db_name = DB_PATH;
  } else {
    ++db_name; // move to first character after ' '
  }

  // Create and open the database
  sqlite3_open(db_name, &db);
  status = sqlite3_errcode(db);

  if (status != SQLITE_OK) {
    fprintf(retval->log, "ERROR: Unable to open file: %s because %s\n\n",
            db_name, sqlite3_errmsg(db));
  } else {
    fprintf(retval->log, "Opened file %s\n", db_name);
  }

  // Set the state for the driver
  retval->port = port;
  retval->db = db;
  retval->key = 42;
  // FIXME Any way to get canonical path to the DB?
  // We need to ensure equal keys for different paths to the same file
  retval->async_handle = 0;
  retval->prepared_stmts = NULL;
  retval->prepared_count = 0;
  retval->prepared_alloc = 0;

  retval->atom_blob = driver_mk_atom("blob");
  retval->atom_error = driver_mk_atom("error");
  retval->atom_columns = driver_mk_atom("columns");
  retval->atom_rows = driver_mk_atom("rows");
  retval->atom_null = driver_mk_atom("null");
  retval->atom_rowid = driver_mk_atom("rowid");
  retval->atom_ok = driver_mk_atom("ok");
  retval->atom_done = driver_mk_atom("done");
  retval->atom_unknown_cmd = driver_mk_atom("unknown_command");

  fflush(retval->log);
  return (ErlDrvData) retval;
}

// Driver Stop
static void stop(ErlDrvData handle) {
  sqlite3_drv_t* driver_data = (sqlite3_drv_t*) handle;
  unsigned int i;

  if (driver_data->prepared_stmts) {
    for (i = 0; i < driver_data->prepared_count; i++) {
      sqlite3_finalize(driver_data->prepared_stmts[i]);
    }
    driver_free(driver_data->prepared_stmts);
  }
  sqlite3_close(driver_data->db);
  fclose(driver_data->log);
  driver_data->log = NULL;

  driver_free(driver_data);
}

// Handle input from Erlang VM
static int control(
    ErlDrvData drv_data, unsigned int command, char *buf,
    int len, char **rbuf, int rlen) {
  sqlite3_drv_t* driver_data = (sqlite3_drv_t*) drv_data;
  int i;
  switch (command) {
  case CMD_SQL_EXEC:
    sql_exec(driver_data, buf, len);
    break;
  case CMD_SQL_BIND_AND_EXEC:
    sql_bind_and_exec(driver_data, buf, len);
    break;
  case CMD_PREPARE:
    prepare(driver_data, buf, len);
    break;
  case CMD_PREPARED_BIND:
    prepared_bind(driver_data, buf, len);
    break;
  case CMD_PREPARED_STEP:
    prepared_step(driver_data, buf, len);
    break;
  case CMD_PREPARED_RESET:
    prepared_reset(driver_data, buf, len);
    break;
  case CMD_PREPARED_CLEAR_BINDINGS:
    prepared_clear_bindings(driver_data, buf, len);
    break;
  case CMD_PREPARED_FINALIZE:
    prepared_finalize(driver_data, buf, len);
    break;
  case CMD_PREPARED_COLUMNS:
    prepared_columns(driver_data, buf, len);
    break;
  default:
    unknown(driver_data, buf, len);
  }
  return 0;
}

static inline int return_error(
    sqlite3_drv_t *drv, int error_code, const char *error,
    ErlDrvTermData **spec, int *term_count) {
  *spec = (ErlDrvTermData *) driver_alloc(11 * sizeof(ErlDrvTermData));
  (*spec)[0] = ERL_DRV_PORT;
  (*spec)[1] = driver_mk_port(drv->port);
  (*spec)[2] = ERL_DRV_ATOM;
  (*spec)[3] = drv->atom_error;
  (*spec)[4] = ERL_DRV_INT;
  (*spec)[5] = error_code;
  (*spec)[6] = ERL_DRV_STRING;
  (*spec)[7] = (ErlDrvTermData) error;
  (*spec)[8] = strlen(error);
  (*spec)[9] = ERL_DRV_TUPLE;
  (*spec)[10] = 4;
  *term_count = 11;
  return 0;
}

static inline int output_error(
    sqlite3_drv_t *drv, int error_code, const char *error) {
  ErlDrvTermData *dataset;
  int term_count;
  return_error(drv, error_code, error, &dataset, &term_count);
  driver_output_term(drv->port, dataset, term_count);
  return 0;
}

static inline int output_db_error(sqlite3_drv_t *drv) {
  return output_error(drv, sqlite3_errcode(drv->db), sqlite3_errmsg(drv->db));
}

static inline int output_ok(sqlite3_drv_t *drv) {
  // Return {Port, ok}
  ErlDrvTermData spec[] = {
      ERL_DRV_PORT, driver_mk_port(drv->port),
      ERL_DRV_ATOM, drv->atom_ok,
      ERL_DRV_TUPLE, 2
  };
  return driver_output_term(drv->port, spec, sizeof(spec) / sizeof(spec[0]));
}

static inline async_sqlite3_command *make_async_command(
    sqlite3_drv_t *drv, sqlite3_stmt *statement) {
  async_sqlite3_command *result =
      (async_sqlite3_command *) driver_alloc(sizeof(async_sqlite3_command));
  memset(result, 0, sizeof(async_sqlite3_command));

  result->driver_data = drv;
  result->statement = statement;
  return result;
}

static inline int sql_exec_statement(
    sqlite3_drv_t *drv, sqlite3_stmt *statement) {
  async_sqlite3_command *async_command = make_async_command(drv, statement);

#ifdef DEBUG
  fprintf(drv->log, "Driver async: %d %p\n", SQLITE_VERSION_NUMBER, async_command->statement);
  fflush(drv->log);
#endif

  if (sqlite3_threadsafe()) {
    drv->async_handle = driver_async(drv->port, &drv->key, sql_exec_async,
                                     async_command, sql_free_async);
  } else {
    sql_exec_async(async_command);
    ready_async((ErlDrvData) drv, (ErlDrvThreadData) async_command);
  }
  return 0;
}

static int sql_exec(sqlite3_drv_t *drv, char *command, int command_size) {
  int result;
  char *rest = NULL;
  sqlite3_stmt *statement;

#ifdef DEBUG
  fprintf(drv->log, "Preexec: %.*s\n", command_size, command);
  fflush(drv->log);
#endif
  result = sqlite3_prepare_v2(drv->db, command, command_size, &statement,
                              (const char **) &rest);
  if (result != SQLITE_OK) {
    return output_db_error(drv);
  }

  return sql_exec_statement(drv, statement);
}

static inline int decode_and_bind_param(
    sqlite3_drv_t *drv, char *buffer, int *p_index,
    sqlite3_stmt *statement, int param_index, int *p_type, int *p_size) {
  int result;
  sqlite3_int64 int64_val;
  double double_val;
  char* char_buf_val;
  long bin_size;

  ei_get_type(buffer, p_index, p_type, p_size);
  switch (*p_type) {
  case ERL_SMALL_INTEGER_EXT:
  case ERL_INTEGER_EXT:
  case ERL_SMALL_BIG_EXT:
  case ERL_LARGE_BIG_EXT:
    ei_decode_longlong(buffer, p_index, &int64_val);
    result = sqlite3_bind_int64(statement, param_index, int64_val);
    break;
  case ERL_FLOAT_EXT:
  case NEW_FLOAT_EXT: // what's the difference?
    ei_decode_double(buffer, p_index, &double_val);
    result = sqlite3_bind_double(statement, param_index, double_val);
    break;
  case ERL_ATOM_EXT:
    // include space for null separator
    char_buf_val = driver_alloc((*p_size + 1) * sizeof(char));
    ei_decode_atom(buffer, p_index, char_buf_val);
    if (strncmp(char_buf_val, "null", 5) == 0) {
      result = sqlite3_bind_null(statement, param_index);
    }
    else {
      output_error(drv, SQLITE_MISUSE, "Non-null atom as parameter");
      return 1;
    }
    break;
  case ERL_STRING_EXT:
    // include space for null separator
    char_buf_val = driver_alloc((*p_size + 1) * sizeof(char));
    ei_decode_string(buffer, p_index, char_buf_val);
    result = sqlite3_bind_text(statement, param_index, char_buf_val, *p_size, &driver_free);
    break;
  case ERL_BINARY_EXT:
    char_buf_val = driver_alloc(*p_size * sizeof(char));
    ei_decode_binary(buffer, p_index, char_buf_val, &bin_size);
    // assert(bin_size == *p_size)
    result = sqlite3_bind_text(statement, param_index, char_buf_val, *p_size, &driver_free);
    break;
  case ERL_SMALL_TUPLE_EXT:
    // assume this is {blob, Blob}
    ei_get_type(buffer, p_index, p_type, p_size);
    ei_decode_tuple_header(buffer, p_index, p_size);
    assert (*p_size == 2);
    ei_skip_term(buffer, p_index); // skipped the atom 'blob'
    ei_get_type(buffer, p_index, p_type, p_size);
    assert (*p_type == ERL_BINARY_EXT);
    char_buf_val = driver_alloc(*p_size * sizeof(char));
    ei_decode_binary(buffer, p_index, char_buf_val, &bin_size);
    // assert(bin_size == *p_size)
    result = sqlite3_bind_blob(statement, param_index, char_buf_val, *p_size, &driver_free);
    break;
  default:
    output_error(drv, SQLITE_MISUSE, "bad parameter type");
    return 1;
  }
  if (result != SQLITE_OK) {
    output_db_error(drv);
    return result;
  }
  return SQLITE_OK;
}

static int bind_parameters(
    sqlite3_drv_t *drv, char *buffer, int buffer_size, int *p_index,
    sqlite3_stmt *statement, int *p_type, int *p_size) {
  // decoding parameters
  int i, cur_list_size = -1, param_index = 1, param_indices_are_explicit = 0, result;
  long param_index_long;
  char param_name[MAXATOMLEN + 1]; // parameter names shouldn't be longer than 256!
  while (*p_index < buffer_size) {
    ei_decode_list_header(buffer, p_index, &cur_list_size);
    for (i = 0; i < cur_list_size; i++) {
      ei_get_type(buffer, p_index, p_type, p_size);
      if (*p_type == ERL_SMALL_TUPLE_EXT) {
        int old_index = *p_index;
        // param with name or explicit index
        param_indices_are_explicit = 1;
        if (*p_size != 2) {
          return output_error(drv, SQLITE_MISUSE,
                              "tuple should contain index or name, and value");
        }
        ei_decode_tuple_header(buffer, p_index, p_size);
        ei_get_type(buffer, p_index, p_type, p_size);
        // first element of tuple is int (index), atom, or string (name)
        switch (*p_type) {
        case ERL_SMALL_INTEGER_EXT:
        case ERL_INTEGER_EXT:
          ei_decode_long(buffer, p_index, &param_index_long);
          param_index = param_index_long;
          break;
        case ERL_ATOM_EXT:
          ei_decode_atom(buffer, p_index, param_name);
          // insert zero terminator
          param_name[*p_size] = '\0';
          if (strncmp(param_name, "blob", 5) == 0) {
            // this isn't really a parameter name!
            *p_index = old_index;
            param_indices_are_explicit = 0;
            goto IMPLICIT_INDEX; // yuck
          }
          else {
            param_index = sqlite3_bind_parameter_index(statement, param_name);
          }
          break;
        case ERL_STRING_EXT:
          if (*p_size >= MAXATOMLEN) {
            return output_error(drv, SQLITE_TOOBIG, "parameter name too long");
          }
          ei_decode_string(buffer, p_index, param_name);
          // insert zero terminator
          param_name[*p_size] = '\0';
          param_index = sqlite3_bind_parameter_index(statement, param_name);
          break;
        default:
          return output_error(
              drv, SQLITE_MISMATCH,
              "parameter index must be given as integer, atom, or string");
        }
        result = decode_and_bind_param(
            drv, buffer, p_index, statement, param_index, p_type, p_size);
        if (result != SQLITE_OK) {
          return result; // error has already been output
        }
      }
      else {
        IMPLICIT_INDEX:
        if (param_indices_are_explicit) {
          return output_error(
              drv, SQLITE_MISUSE,
              "parameters without indices shouldn't follow indexed or named parameters");
        }

        result = decode_and_bind_param(
            drv, buffer, p_index, statement, param_index, p_type, p_size);
        if (result != SQLITE_OK) {
          return result; // error has already been output
        }
        ++param_index;
      }
    }
  }
  return result;
}

static void get_columns(
    sqlite3_drv_t *drv, sqlite3_stmt *statement, int column_count, int base,
    int *p_term_count, int *p_term_allocated, ErlDrvTermData **p_dataset) {
  int i;

  *p_term_count += column_count * 3 + 3;
  if (*p_term_count > *p_term_allocated) {
    *p_term_allocated = max(*p_term_count, (*p_term_allocated)*2);
    *p_dataset = driver_realloc(*p_dataset, sizeof(ErlDrvTermData) * *p_term_allocated);
  }
  for (i = 0; i < column_count; i++) {
    char *column_name = (char *) sqlite3_column_name(statement, i);
#ifdef DEBUG
    fprintf(drv->log, "Column: %s\n", column_name);
    fflush(drv->log);
#endif

    (*p_dataset)[base + (i * 3)] = ERL_DRV_STRING;
    (*p_dataset)[base + (i * 3) + 1] = (ErlDrvTermData) column_name;
    (*p_dataset)[base + (i * 3) + 2] = strlen(column_name);
  }
  (*p_dataset)[base + column_count * 3 + 0] = ERL_DRV_NIL;
  (*p_dataset)[base + column_count * 3 + 1] = ERL_DRV_LIST;
  (*p_dataset)[base + column_count * 3 + 2] = column_count + 1;
}

static int sql_bind_and_exec(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  int result;
  int index = 0;
  int type, size;
  char *rest = NULL;
  sqlite3_stmt *statement;
  long bin_size;

#ifdef DEBUG
  fprintf(drv->log, "Preexec: %.*s\n", buffer_size, buffer);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  result = ei_decode_tuple_header(buffer, &index, &size);
  if (size != 2) {
    return output_error(drv, SQLITE_MISUSE,
                        "Expected a tuple of SQL command and params");
  }

  // decode SQL statement
  ei_get_type(buffer, &index, &type, &size);
  // TODO support any iolists
  if (type != ERL_BINARY_EXT) {
    return output_error(drv, SQLITE_MISUSE,
                        "SQL should be sent as an Erlang binary");
  }

  char *command = driver_alloc(size * sizeof(char));
  ei_decode_binary(buffer, &index, command, &bin_size);
  // assert(bin_size == size)
  result = sqlite3_prepare_v2(drv->db, command, size, &statement,
                              (const char **) &rest);
  driver_free(command);

  if (result != SQLITE_OK) {
    return output_db_error(drv);
  }

  result = bind_parameters(drv, buffer, buffer_size, &index, statement, &type, &size);
  if (result == SQLITE_OK) {
    return sql_exec_statement(drv, statement);
  } else {
    return result; // error has already been output
  }
}

static void sql_free_async(void *_async_command) {
  async_sqlite3_command *async_command =
      (async_sqlite3_command *) _async_command;
  driver_free(async_command->dataset);

  async_command->driver_data->async_handle = 0;

  free_ptr_list(async_command->ptrs, &driver_free);

  free_ptr_list(async_command->binaries,
                (void (*)(void *)) &driver_free_binary);

  if (async_command->finalize_statement_on_free && async_command->statement) {
    sqlite3_finalize(async_command->statement);
    async_command->statement = NULL;
  }
  driver_free(async_command);
}

static void sql_exec_async(void *_async_command) {
  async_sqlite3_command *async_command =
      (async_sqlite3_command *) _async_command;
  int term_count = 0;
  int term_allocated = 0;
  ErlDrvTermData *dataset = NULL;
  int row_count = 0;
  sqlite3_drv_t *drv = async_command->driver_data;

  int next_row, column_count;
  sqlite3_stmt *statement = async_command->statement;

  ptr_list *ptrs = NULL;
  ptr_list *binaries = NULL;
  int i;

  column_count = sqlite3_column_count(statement);

  term_count += 2;
  if (term_count > term_allocated) {
    term_allocated = max(term_count, term_allocated*2);
    dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
  }
  dataset[term_count - 2] = ERL_DRV_PORT;
  dataset[term_count - 1] = driver_mk_port(drv->port);

  if (column_count > 0) {
    term_count += 2;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 2] = ERL_DRV_ATOM;
    dataset[term_count - 1] = drv->atom_columns;
    int base_term_count = term_count;
    get_columns(
        drv, statement, column_count, base_term_count, &term_count, &term_allocated, &dataset);
    term_count += 4;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[base_term_count + column_count * 3 + 3] = ERL_DRV_TUPLE;
    dataset[base_term_count + column_count * 3 + 4] = 2;

    dataset[base_term_count + column_count * 3 + 5] = ERL_DRV_ATOM;
    dataset[base_term_count + column_count * 3 + 6] = drv->atom_rows;
  }

#ifdef DEBUG
  fprintf(drv->log, "Exec: %s\n", sqlite3_sql(statement));
  fflush(drv->log);
#endif

  while ((next_row = sqlite3_step(statement)) == SQLITE_ROW) {
    for (i = 0; i < column_count; i++) {
#ifdef DEBUG
      fprintf(drv->log, "Column %d type: %d\n", i, sqlite3_column_type(statement, i));
      fflush(drv->log);
#endif
      switch (sqlite3_column_type(statement, i)) {
      case SQLITE_INTEGER: {
        ErlDrvSInt64 *int64_ptr = driver_alloc(sizeof(ErlDrvSInt64));
        *int64_ptr = (ErlDrvSInt64) sqlite3_column_int64(statement, i);
        ptrs = add_to_ptr_list(ptrs, int64_ptr);

        term_count += 2;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 2] = ERL_DRV_INT64;
        dataset[term_count - 1] = (ErlDrvTermData) int64_ptr;
        break;
      }
      case SQLITE_FLOAT: {
        double *float_ptr = driver_alloc(sizeof(double));
        *float_ptr = sqlite3_column_double(statement, i);
        ptrs = add_to_ptr_list(ptrs, float_ptr);

        term_count += 2;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 2] = ERL_DRV_FLOAT;
        dataset[term_count - 1] = (ErlDrvTermData) float_ptr;
        break;
      }
      case SQLITE_BLOB: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        binaries = add_to_ptr_list(binaries, binary);

        term_count += 8;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 8] = ERL_DRV_ATOM;
        dataset[term_count - 7] = drv->atom_blob;
        dataset[term_count - 6] = ERL_DRV_BINARY;
        dataset[term_count - 5] = (ErlDrvTermData) binary;
        dataset[term_count - 4] = bytes;
        dataset[term_count - 3] = 0;
        dataset[term_count - 2] = ERL_DRV_TUPLE;
        dataset[term_count - 1] = 2;
        break;
      }
      case SQLITE_TEXT: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        binaries = add_to_ptr_list(binaries, binary);

        term_count += 4;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 4] = ERL_DRV_BINARY;
        dataset[term_count - 3] = (ErlDrvTermData) binary;
        dataset[term_count - 2] = bytes;
        dataset[term_count - 1] = 0;
        break;
      }
      case SQLITE_NULL: {
        term_count += 2;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 2] = ERL_DRV_ATOM;
        dataset[term_count - 1] = drv->atom_null;
        break;
      }
      }
    }
    term_count += 2;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 2] = ERL_DRV_TUPLE;
    dataset[term_count - 1] = column_count;

    row_count++;
  }
  async_command->finalize_statement_on_free = 1;
  async_command->row_count = row_count;
  async_command->ptrs = ptrs;
  async_command->binaries = binaries;

  if (next_row == SQLITE_BUSY) {
    return_error(drv, SQLITE_BUSY, "SQLite3 database is busy",
                 &async_command->dataset, &async_command->term_count);
    return;
  }
  if (next_row != SQLITE_DONE) {
    return_error(drv, next_row, sqlite3_errmsg(drv->db),
                 &async_command->dataset, &async_command->term_count);
    return;
  }

  if (column_count > 0) {
    term_count += 3+2+3;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 8] = ERL_DRV_NIL;
    dataset[term_count - 7] = ERL_DRV_LIST;
    dataset[term_count - 6] = row_count + 1;

    dataset[term_count - 5] = ERL_DRV_TUPLE;
    dataset[term_count - 4] = 2;

    dataset[term_count - 3] = ERL_DRV_NIL;
    dataset[term_count - 2] = ERL_DRV_LIST;
    dataset[term_count - 1] = 3;
  } else if (sql_is_insert(sqlite3_sql(statement))) {
    sqlite3_int64 rowid = sqlite3_last_insert_rowid(drv->db);
    term_count += 6;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 6] = ERL_DRV_ATOM;
    dataset[term_count - 5] = drv->atom_rowid;
    dataset[term_count - 4] = ERL_DRV_INT;
    dataset[term_count - 3] = rowid;
    dataset[term_count - 2] = ERL_DRV_TUPLE;
    dataset[term_count - 1] = 2;
  } else {
    term_count += 2;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 2] = ERL_DRV_ATOM;
    dataset[term_count - 1] = drv->atom_ok;
  }

  term_count += 2;
  if (term_count > term_allocated) {
    term_allocated = max(term_count, term_allocated*2);
    dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
  }
  dataset[term_count - 2] = ERL_DRV_TUPLE;
  dataset[term_count - 1] = 2;

  async_command->dataset = dataset;
  async_command->term_count = term_count;
#ifdef DEBUG
  fprintf(drv->log, "Total term count: %p %d, rows count: %dx%d\n", statement, term_count, column_count, row_count);
  fflush(drv->log);
#endif
}

static void sql_step_async(void *_async_command) {
  async_sqlite3_command *async_command =
      (async_sqlite3_command *) _async_command;
  int term_count = 0;
  int term_allocated = 0;
  ErlDrvTermData *dataset = NULL;
  sqlite3_drv_t *drv = async_command->driver_data;

  int column_count;
  sqlite3_stmt *statement = async_command->statement;

  ptr_list *ptrs = NULL;
  ptr_list *binaries = NULL;
  int i;
  int result;

  switch(result = sqlite3_step(statement)) {
  case SQLITE_ROW:
    column_count = sqlite3_column_count(statement);
    term_count += 2;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 2] = ERL_DRV_PORT;
    dataset[term_count - 1] = driver_mk_port(drv->port);

    for (i = 0; i < column_count; i++) {
#ifdef DEBUG
      fprintf(drv->log, "Column %d type: %d\n", i, sqlite3_column_type(statement, i));
      fflush(drv->log);
#endif
      switch (sqlite3_column_type(statement, i)) {
      case SQLITE_INTEGER: {
        ErlDrvSInt64 *int64_ptr = driver_alloc(sizeof(ErlDrvSInt64));
        *int64_ptr = (ErlDrvSInt64) sqlite3_column_int64(statement, i);
        ptrs = add_to_ptr_list(ptrs, int64_ptr);

        term_count += 2;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 2] = ERL_DRV_INT64;
        dataset[term_count - 1] = (ErlDrvTermData) int64_ptr;
        break;
      }
      case SQLITE_FLOAT: {
        double *float_ptr = driver_alloc(sizeof(double));
        *float_ptr = sqlite3_column_double(statement, i);
        ptrs = add_to_ptr_list(ptrs, float_ptr);

        term_count += 2;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 2] = ERL_DRV_FLOAT;
        dataset[term_count - 1] = (ErlDrvTermData) float_ptr;
        break;
      }
      case SQLITE_BLOB: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        binaries = add_to_ptr_list(binaries, binary);

        term_count += 8;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 8] = ERL_DRV_ATOM;
        dataset[term_count - 7] = drv->atom_blob;
        dataset[term_count - 6] = ERL_DRV_BINARY;
        dataset[term_count - 5] = (ErlDrvTermData) binary;
        dataset[term_count - 4] = bytes;
        dataset[term_count - 3] = 0;
        dataset[term_count - 2] = ERL_DRV_TUPLE;
        dataset[term_count - 1] = 2;
        break;
      }
      case SQLITE_TEXT: {
        int bytes = sqlite3_column_bytes(statement, i);
        ErlDrvBinary* binary = driver_alloc_binary(bytes);
        binary->orig_size = bytes;
        memcpy(binary->orig_bytes,
               sqlite3_column_blob(statement, i), bytes);
        binaries = add_to_ptr_list(binaries, binary);

        term_count += 4;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 4] = ERL_DRV_BINARY;
        dataset[term_count - 3] = (ErlDrvTermData) binary;
        dataset[term_count - 2] = bytes;
        dataset[term_count - 1] = 0;
        break;
      }
      case SQLITE_NULL: {
        term_count += 2;
        if (term_count > term_allocated) {
          term_allocated = max(term_count, term_allocated*2);
          dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
        }
        dataset[term_count - 2] = ERL_DRV_ATOM;
        dataset[term_count - 1] = drv->atom_null;
        break;
      }
      }
    }
    term_count += 2;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 2] = ERL_DRV_TUPLE;
    dataset[term_count - 1] = column_count;

    async_command->ptrs = ptrs;
    async_command->binaries = binaries;
    break;
  case SQLITE_DONE:
    term_count += 4;
    if (term_count > term_allocated) {
      term_allocated = max(term_count, term_allocated*2);
      dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
    }
    dataset[term_count - 4] = ERL_DRV_PORT;
    dataset[term_count - 3] = driver_mk_port(drv->port);
    dataset[term_count - 2] = ERL_DRV_ATOM;
    dataset[term_count - 1] = drv->atom_done;
    sqlite3_reset(statement);
    break;
  case SQLITE_BUSY:
    return_error(drv, SQLITE_BUSY, "SQLite3 database is busy",
                 &dataset, &term_count);
    sqlite3_reset(statement);
    goto POPULATE_COMMAND;
    break;
  default:
    return_error(drv, result, sqlite3_errmsg(drv->db),
                 &dataset, &term_count);
    sqlite3_reset(statement);
    goto POPULATE_COMMAND;
  }

  term_count += 2;
  if (term_count > term_allocated) {
    term_allocated = max(term_count, term_allocated*2);
    dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
  }
  dataset[term_count - 2] = ERL_DRV_TUPLE;
  dataset[term_count - 1] = 2;

POPULATE_COMMAND:
  async_command->dataset = dataset;
  async_command->term_count = term_count;
  async_command->ptrs = ptrs;
  async_command->binaries = binaries;
  async_command->row_count = 1;
#ifdef DEBUG
  fprintf(drv->log, "Total term count: %p %d, rows count: %dx%d\n", statement, term_count, column_count, row_count);
  fflush(drv->log);
#endif
}

static void ready_async(ErlDrvData drv_data, ErlDrvThreadData thread_data) {
  async_sqlite3_command *async_command =
      (async_sqlite3_command *) thread_data;
  sqlite3_drv_t *drv = async_command->driver_data;

  int res = driver_output_term(drv->port,
                               async_command->dataset,
                               async_command->term_count);
  (void) res; // suppress unused warning
#ifdef DEBUG
  fprintf(drv->log, "Total term count: %p %d, rows count: %d (%d)\n", async_command->statement, async_command->term_count, async_command->row_count, res);
  fflush(drv->log);
#endif
  sql_free_async(async_command);
}

static int prepare(sqlite3_drv_t *drv, char *command, int command_size) {
  int result;
  char *rest = NULL;
  sqlite3_stmt *statement;

#ifdef DEBUG
  fprintf(drv->log, "Preparing statement: %.*s\n", command_size, command);
  fflush(drv->log);
#endif
  result = sqlite3_prepare_v2(drv->db, command, command_size, &statement,
                              (const char **) &rest);
  if (result != SQLITE_OK) {
    return output_db_error(drv);
  }

  if (drv->prepared_count >= drv->prepared_alloc) {
    drv->prepared_alloc =
        (drv->prepared_alloc != 0) ? 2*drv->prepared_alloc : 4;
    drv->prepared_stmts =
        driver_realloc(drv->prepared_stmts,
                       drv->prepared_alloc * sizeof(sqlite3_stmt *));
  }
  drv->prepared_stmts[drv->prepared_count] = statement;
  drv->prepared_count++;

  ErlDrvTermData spec[] = {
      ERL_DRV_PORT, driver_mk_port(drv->port),
      ERL_DRV_UINT, drv->prepared_count - 1,
      ERL_DRV_TUPLE, 2
  };
  return driver_output_term(drv->port, spec, sizeof(spec) / sizeof(spec[0]));
}

static int prepared_bind(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  int result;
  long long_prepared_index;
  int index = 0, type, size;

#ifdef DEBUG
  fprintf(drv->log, "Finalizing prepared statement: %.*s\n", buffer_size, buffer);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  ei_decode_tuple_header(buffer, &index, &size);
  // assert(size == 2);
  ei_decode_long(buffer, &index, &long_prepared_index);
  unsigned int prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to bind non-existent prepared statement");
  }

  sqlite3_stmt *statement = drv->prepared_stmts[prepared_index];
  result =
      bind_parameters(drv, buffer, buffer_size, &index, statement, &type, &size);
  if (result == SQLITE_OK) {
    return output_ok(drv);
  } else {
    return result; // error has already been output
  }
}

static int prepared_columns(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  long long_prepared_index;
  int index = 0, term_count = 0, term_allocated = 0;

#ifdef DEBUG
  fprintf(drv->log, "Finalizing prepared statement: %.*s\n", buffer_size, buffer);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  unsigned int prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to reset non-existent prepared statement");
  }

  sqlite3_stmt *statement = drv->prepared_stmts[prepared_index];
  ErlDrvTermData *dataset = NULL;

  term_count += 2;
  if (term_count > term_allocated) {
    term_allocated = max(term_count, term_allocated*2);
    dataset = driver_realloc(dataset, sizeof(ErlDrvTermData) * term_allocated);
  }
  dataset[term_count - 2] = ERL_DRV_PORT;
  dataset[term_count - 1] = driver_mk_port(drv->port);

  int column_count = sqlite3_column_count(statement);

  get_columns(
      drv, statement, column_count, 2, &term_count, &term_allocated, &dataset);
  term_count += 2;
  dataset[term_count - 2] = ERL_DRV_TUPLE;
  dataset[term_count - 1] = 2;

  return driver_output_term(drv->port, dataset, term_count);
}

static int prepared_step(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  int result;
  long long_prepared_index;
  int index = 0;
  char *rest = NULL;

#ifdef DEBUG
  fprintf(drv->log, "Evaluating prepared statement: %.*s\n", command_size, command);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  unsigned int prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to evaluate non-existent prepared statement");
  }

  sqlite3_stmt *statement = drv->prepared_stmts[prepared_index];
  async_sqlite3_command *async_command = make_async_command(drv, statement);

  if (sqlite3_threadsafe()) {
    drv->async_handle = driver_async(drv->port, &drv->key, sql_step_async,
                                     async_command, sql_free_async);
  } else {
    sql_exec_async(async_command);
    ready_async((ErlDrvData) drv, (ErlDrvThreadData) async_command);
  }
  return 0;
}

static int prepared_reset(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  long long_prepared_index;
  int index = 0;

#ifdef DEBUG
  fprintf(drv->log, "Finalizing prepared statement: %.*s\n", command_size, command);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  unsigned int prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to reset non-existent prepared statement");
  }

  // don't bother about error code, any errors should already be shown by step
  sqlite3_stmt *statement = drv->prepared_stmts[prepared_index];
  sqlite3_reset(statement);
  return output_ok(drv);
}

static int prepared_clear_bindings(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  long long_prepared_index;
  int index = 0;

#ifdef DEBUG
  fprintf(drv->log, "Finalizing prepared statement: %.*s\n", command_size, command);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  unsigned int prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to clear bindings of non-existent prepared statement");
  }

  sqlite3_stmt *statement = drv->prepared_stmts[prepared_index];
  sqlite3_clear_bindings(statement);
  return output_ok(drv);
}

static int prepared_finalize(sqlite3_drv_t *drv, char *buffer, int buffer_size) {
  long long_prepared_index;
  int index = 0;

#ifdef DEBUG
  fprintf(drv->log, "Finalizing prepared statement: %.*s\n", command_size, command);
  fflush(drv->log);
#endif

  ei_decode_version(buffer, &index, NULL);
  ei_decode_long(buffer, &index, &long_prepared_index);
  unsigned int prepared_index = (unsigned int) long_prepared_index;

  if (prepared_index >= drv->prepared_count) {
    return output_error(drv, SQLITE_MISUSE,
                        "Trying to finalize non-existent prepared statement");
  }

  // finalize the statement and make sure it isn't accidentally executed again
  sqlite3_finalize(drv->prepared_stmts[prepared_index]);
  drv->prepared_stmts[prepared_index] = NULL;

  // if the statement is at the end of the array, space can be reused;
  // otherwise don't bother
  if (prepared_index == drv->prepared_count - 1) {
    drv->prepared_count--;
  }
  return output_ok(drv);
}

// Unknown Command
static int unknown(sqlite3_drv_t *drv, char *command, int command_size) {
  // Return {Port, error, unknown_command}
  ErlDrvTermData spec[] = {
      ERL_DRV_PORT, driver_mk_port(drv->port),
      ERL_DRV_ATOM, drv->atom_error,
      ERL_DRV_INT, -1,
      ERL_DRV_ATOM, drv->atom_unknown_cmd,
      ERL_DRV_TUPLE, 4
  };
  return driver_output_term(drv->port, spec, sizeof(spec) / sizeof(spec[0]));
}

static inline ptr_list *add_to_ptr_list(ptr_list *list, void *value_ptr) {
  ptr_list* new_node = driver_alloc(sizeof(ptr_list));
  new_node->head = value_ptr;
  new_node->tail = NULL;
  if (list) {
    list->tail = new_node;
    return list;
  } else {
    return new_node;
  }
}

static inline void free_ptr_list(ptr_list *list, void(* free_head)(void *)) {
  ptr_list* tail;
  while (list) {
    tail = list->tail;
    (*free_head)(list->head);
    driver_free(list);
    list = tail;
  }
}

static inline int max(int a, int b) {
  return a >= b ? a : b;
}

static inline int sql_is_insert(const char *sql) {
  // neither strcasestr nor strnicmp are portable, so have to do this
  int i;
  char *insert = "insert";
  for (i = 0; i < 6; i++) {
    if ((tolower(sql[i]) != insert[i]) && (sql[i] != ' '))
      return 0;
  }
  return 1;
}
