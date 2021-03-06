#include "parser_internal.h"
#include "cue_internal.h"
#include <string.h>

#define _ERROR(X) do { if( skip_error == 0 ) { ERROR(X); } } while(0)

/**
 * ASCII characters
 */
#define  ASCII_PERIOD (0x2E)
#define  ASCII_CR     (0x0D)
#define  ASCII_LF     (0x0A)
#define  ASCII_SPACE  (0x20)
#define  ASCII_TAB    (0x09)
#define  ASCII_DASH   (0x2D)
#define  ASCII_GT     (0x3E)

/**
 * parser modes
 */
enum
{
	M_WEBVTT = 0,
	M_CUETEXT,
	M_SKIP_CUE,
	M_READ_LINE,
};


/**
 * states
 */
enum parse_state_t
{
	/**
	 * WEBVTT parse states
	 */
		T_INITIAL = 0,
		T_TAG,
		T_TAGCOMMENT,
		T_EOL,
		T_BODY,

		T_CUEREAD, /* Read a line of text for a cue */
		T_CUE, /* T_CUEID T_CUEPARAMS T_CUETEXT NEWLINE */
		T_CUEID, /* T_LINE !~ SEPARATOR && LINE !~ ^NOTE NEWLINE */
		T_CUEPARAMS, /* TIMESTAMP WHITESPACE? SEPARATOR WHITESPACE? T_CUESETTING* NEWLINE */
		T_CUETEXT, /* T_LINE !~ SEPARATOR NEWLINE NEWLINE */

		T_TIMESTAMP, /* This looked like a timestamp to the lexer, may or may not be valid. */

		/**
		 * NOTE comments
		 */
		T_COMMENT,

		/**
		 * Cuetimes
		 */
		T_FROM,
		T_SEP_LEFT,
		T_SEP,
		T_SEP_RIGHT,
		T_UNTIL,

		/**
		 * Cuesettings
		 */
		T_PRECUESETTING,
		T_CUESETTING,
		T_CUESETTING_DELIMITER,
		T_CUESETTING_VALUE,
		T_SKIP_SETTING /* We have to skip a cue-setting because of an error. */

	/**
	 * Cuetext parse states
	 */
};

#define ASCII_DASH (0x2D)
#define ASCII_COLON  (0x3A)

#define MSECS_PER_HOUR (3600000)
#define MSECS_PER_MINUTE (60000)
#define MSECS_PER_SECOND (1000)
#define BUFFER (self->buffer + self->position)
#define MALFORMED_TIME ((webvtt_timestamp_t)-1.0)

static int find_bytes(const webvtt_byte *buffer, webvtt_uint len, const webvtt_byte *sbytes, webvtt_uint slen);
static webvtt_int64 parse_int( const webvtt_byte **pb, int *pdigits );
static int parse_timestamp( webvtt_parser self, const webvtt_byte *b, webvtt_timestamp *result );

WEBVTT_EXPORT webvtt_status
webvtt_create_parser( webvtt_cue_fn_ptr on_read,
						webvtt_error_fn_ptr on_error, void *
						userdata,
						webvtt_parser *ppout )
{
	webvtt_parser p;
	if( !on_read || !on_error || !ppout )
	{
		return WEBVTT_INVALID_PARAM;
	}
	
	if( !(p = (webvtt_parser)webvtt_alloc0( sizeof *p ) ) )
	{
		return WEBVTT_OUT_OF_MEMORY;
	}
	
	memset(p->astack, 0, sizeof(p->astack));
	p->stack = p->astack;
	p->top = p->stack;
	p->top->state = T_INITIAL;
	p->stack_alloc = sizeof(p->astack) / sizeof(p->astack[0]);

	p->read = on_read;
	p->error = on_error;
	p->column = p->line = 1;
	p->userdata = userdata;
	*ppout = p;
	
	return WEBVTT_SUCCESS;
}

static void
finish_cue( webvtt_parser self, webvtt_cue *cue )
{
	if( cue && *cue )
	{
		/**
		 * Validate the cue
		 */
		if( webvtt_validate_cue( (webvtt_cue)*cue ) )
		{
			self->read( self->userdata, *cue );
		}
		else
		{
			webvtt_release_cue( cue );
		}
		*cue = 0;
	}
}

/**
 * This routine tries to clean up the stack
 * for us, to prevent leaks.
 *
 * It should also help find errors in stack management.
 */
WEBVTT_INTERN void
cleanup_stack( webvtt_parser self )
{
	webvtt_state *st = self->top;
	while( st >= self->stack )
	{
		switch( st->type )
		{
			case V_CUE: webvtt_release_cue( &st->v.cue ); break;
			case V_TEXT: webvtt_delete_bytearray( &st->v.text ); break;
			/**
			 * TODO: Clean up cuetext nodes as well.
			 * Eventually the cuetext parser will probably be making use
			 * of this stack, and will need to manage it well also.
			 */
		}
		st->type = V_NONE;
		st->line = st->column = st->v.value = st->token = 0;
		if( st > self->stack )
		{
			--self->top;
		}
		--st;
	}
	if( self->stack != self->astack )
	{
		/**
		 * If the stack is dynamically allocated (probably not),
		 * then point it to the statically allocated one (and zeromem it),
		 * then finally delete the old dynamically allocated stack
		 */
		webvtt_state *pst = self->stack;
		memset( self->astack, 0, sizeof(self->astack) );
		self->stack = self->astack;
		self->stack_alloc = sizeof(self->astack) / sizeof(*(self->astack));
		webvtt_free( pst );
	}
}

WEBVTT_EXPORT void
webvtt_delete_parser( webvtt_parser self )
{
	if( self )
	{
		cleanup_stack( self );

		if( self->line_buffer )
		{
			webvtt_delete_bytearray( &self->line_buffer );
		}
		webvtt_free( self );
	}
}

#define BEGIN_STATE(State) case State: {
#define END_STATE } break;
#define IF_TOKEN(Token,Actions) case Token: { Actions } break;
#define BEGIN_DFA switch(top->state) {
#define END_DFA }
#define BEGIN_TOKEN switch(token) {
#define END_TOKEN }
#define IF_TRANSITION(Token,State) if( token == Token ) { self->state = State;
#define ELIF_TRANSITION(Token,State) } else IF_TRANSITION(Token,State)
#define ENDIF }
#define ELSE } else {

static WEBVTT_INTERN int
find_newline( const webvtt_byte *buffer, webvtt_uint *pos, webvtt_uint len )
{
	while( *pos < len )
	{
		if( buffer[ *pos ] == ASCII_CR || buffer[ *pos ] == ASCII_LF )
		{
			return 1;
		} 
		else
		{ 
			(*pos)++;
		}
	}
	return -1;
}

static WEBVTT_INTERN void
find_next_whitespace(const webvtt_byte *buffer, webvtt_uint *ppos, webvtt_uint len)
{
	webvtt_uint pos = *ppos;
	while( pos < len )
	{
		webvtt_byte c = buffer[pos];
		if( c == ASCII_CR || c == ASCII_LF || c == ASCII_SPACE || c == ASCII_TAB )
		{
			break;
		}

		++pos;
	}
	*ppos = pos;
}

/**
 * basic strnstr-ish routine
 */
WEBVTT_INTERN int
find_bytes(const webvtt_byte *buffer, webvtt_uint len, const webvtt_byte *sbytes, webvtt_uint slen)
{
	webvtt_uint slen2 = slen - 1;
	do
	{
		if( *buffer == *sbytes && memcmp( buffer + 1, sbytes + 1, slen2) == 0 )
		{
			return 1;
		}
	} while( len-- >= slen && *buffer++ );
	return 0;
}

	/**
	 * Helpers to figure out what state we're on
	 */
#define SP (self->top)
#define AT_BOTTOM (self->top == self->stack)
#define ON_HEAP (self->stack_alloc == sizeof(p->astack) / sizeof(p->astack[0]))
#define STACK_SIZE ((webvtt_uint)(self->top - self->stack))
#define FRAME(i) (self->top - (i))
#define FRAMEUP(i) (self->top + (i))
#define RECHECK goto _recheck;
#define BACK (SP->back)
	/**
	 * More state stack helpers
	 */
static WEBVTT_INTERN webvtt_status
do_push( webvtt_parser self, webvtt_uint token, webvtt_uint back, webvtt_uint state, void *data, webvtt_state_value_type type, webvtt_uint line, webvtt_uint column )
{
	if( STACK_SIZE + 1 >= self->stack_alloc )
	{
		webvtt_state *stack = (webvtt_state *)webvtt_alloc0( sizeof(webvtt_state) * (self->stack_alloc<<1) ), *tmp;
		if( !stack )
		{
			ERROR(WEBVTT_ALLOCATION_FAILED);
			return WEBVTT_OUT_OF_MEMORY;
		}
		memcpy( stack, self->stack, sizeof(webvtt_state) * self->stack_alloc );
		tmp = self->stack;
		self->stack = stack;
		self->top = stack + (self->top - tmp);
		if( tmp != self->astack )
		{
			webvtt_free( tmp );
		}
	}
	++self->top;
	self->top->state = state;
	self->top->type = type;
	self->top->token = token;
	self->top->line = line;
	self->top->back = back;
	self->top->column = column;
	self->top->v.cue = (webvtt_cue)data;
	return WEBVTT_SUCCESS;
}
static WEBVTT_INTERN int
do_pop( webvtt_parser self )
{
	int count = self->top->back;
	self->top -= count;
	self->top->back = 0;
	self->popped = 1;
	return count;
}

#define PUSH0(S,V,T) \
do { \
		self->popped = 0; \
		if( do_push(self,token,BACK+1,(S),(void*)(V),T,last_line, last_column) \
			== WEBVTT_OUT_OF_MEMORY ) \
			return WEBVTT_OUT_OF_MEMORY; \
	} while(0)

#define PUSH(S,B,V,T) \
do { \
	self->popped = 0; \
	if( do_push(self,token,(B),(S),(void*)(V),T,last_line, last_column) \
		== WEBVTT_OUT_OF_MEMORY ) \
		return WEBVTT_OUT_OF_MEMORY; \
	} while(0)

#define POP() \
do \
{ \
	--(self->top); \
	self->popped = 1; \
} while(0)
#define POPBACK() do_pop(self)

WEBVTT_INTERN int
parse_cueparams( webvtt_parser self, const webvtt_byte *buffer,
	webvtt_uint len, webvtt_cue cue )
{
	int digits;
	int have_ws = 0;
	int unexpected_whitespace = 0;
	webvtt_uint baddelim = 0;
	webvtt_uint pos = 0;
	webvtt_token last_token = 0;
	enum cp_state
	{
		CP_T1, CP_T2, CP_T3, CP_T4, CP_T5, /* 'start' cuetime, whitespace1, 
									 'separator', whitespace2, 'end' cuetime */
		CP_CS0, /* pre-cuesetting */

		CP_SD, /* cuesettings delimiter here */

		CP_V1, /* 'vertical' cuesetting */
		CP_P1, /* 'position' cuesetting */
		CP_A1, /* 'align' cuesetting */
		CP_S1, /* 'size' cuesetting */
		CP_L1, /* 'line' cuesetting */

		CP_SV, /* cuesettings value here */

		CP_V2,
		CP_P2,
		CP_A2,
		CP_S2,
		CP_L2,
	};

	enum cp_state last_state = CP_T1;
	enum cp_state state = CP_T1;

#define SETST(X) do { baddelim = 0; last_state = state; state = (X); } while( 0 )

	self->token_pos = 0;
	while( pos < len )
	{
		webvtt_uint last_column = self->column;
		webvtt_token token = webvtt_lex( self, buffer, &pos, len, 1 );
_recheck:
		switch( state )
		{
		/* start timestamp */
		case CP_T1:
			if( token == WHITESPACE && !unexpected_whitespace)
			{
				ERROR_AT_COLUMN(WEBVTT_UNEXPECTED_WHITESPACE, self->column);
				unexpected_whitespace = 1;
			}
			else if( token == TIMESTAMP )
				if( !parse_timestamp(self, self->token, &cue->from ) )
				{
					ERROR_AT_COLUMN(
						( BAD_TIMESTAMP(cue->from) 
						? WEBVTT_EXPECTED_TIMESTAMP 
						: WEBVTT_MALFORMED_TIMESTAMP ),last_column  );
					if( !ASCII_ISDIGIT(self->token[self->token_pos-1]) )
					{
						while( pos < len && buffer[pos] != 0x09 && buffer[pos] != 0x20 ) ++pos;	
					}
					if( BAD_TIMESTAMP(cue->from) )
						return -1;
					SETST(CP_T2);
				}
				else
				{
					SETST(CP_T2);
				}
			else
			{
				ERROR_AT_COLUMN(WEBVTT_EXPECTED_TIMESTAMP,last_column);
				return -1;
			}
			break;
		/* end timestamp */
		case CP_T5:
			_t5:
			if( token == WHITESPACE )
			{
				/* no problem, just ignore it and continue */
			}
			else if( token == TIMESTAMP )
				if( !parse_timestamp(self, self->token, &cue->until ) )
				{
					ERROR_AT_COLUMN(
						( BAD_TIMESTAMP(cue->until) 
						? WEBVTT_EXPECTED_TIMESTAMP 
						: WEBVTT_MALFORMED_TIMESTAMP ),last_column  );
					if( !ASCII_ISDIGIT(self->token[self->token_pos-1]) )
					{
						while( pos < len && buffer[pos] != 0x09 && buffer[pos] != 0x20 ) ++pos;	
					}
					if( BAD_TIMESTAMP(cue->until) )
						return -1;
					SETST(CP_CS0);
				}
				else
				{
					SETST(CP_CS0);
				}
			else
			{
				ERROR_AT_COLUMN(WEBVTT_EXPECTED_TIMESTAMP,last_column);
				return -1;
			}
			break;

		/* whitespace 1 */
		case CP_T2:
			switch(token)
			{
			case SEPARATOR:
				ERROR_AT_COLUMN(WEBVTT_EXPECTED_WHITESPACE,last_column);
				SETST(CP_T4);
				break;
			case WHITESPACE:
				SETST(CP_T3);
				break;
			}
			break;
		case CP_T3:
			switch(token)
			{
			case WHITESPACE: /* ignore this whitespace */
				break;

			case SEPARATOR:
				SETST(CP_T4);
				break;

			case TIMESTAMP:
				ERROR(WEBVTT_MISSING_CUETIME_SEPARATOR);
				SETST(CP_T5);
				goto _recheck;

			default: /* some garbage */
				ERROR_AT_COLUMN(WEBVTT_EXPECTED_CUETIME_SEPARATOR,last_column);
				return -1;
			}
			break;
		case CP_T4:
			switch(token)
			{
			case WHITESPACE:
				SETST(CP_T5);
				break;
			case TIMESTAMP:
				ERROR_AT_COLUMN(WEBVTT_EXPECTED_WHITESPACE,last_column);
				goto _recheck;
			default:
				ERROR_AT_COLUMN(WEBVTT_EXPECTED_WHITESPACE,last_column);
				goto _recheck;
			}
			break;
#define CHKDELIM \
if( baddelim ) \
	ERROR_AT_COLUMN(WEBVTT_INVALID_CUESETTING_DELIMITER,baddelim); \
else if( !have_ws ) \
	ERROR_AT_COLUMN(WEBVTT_EXPECTED_WHITESPACE,last_column);

		/**
		 * This section is "pre-cuesetting". We are expecting whitespace, followed by
		 * a cuesetting keyword
		 *
		 * If we don't see a keyword, but have our whitespace, it is considered a bad keyword
		 * (invalid cuesetting)
		 *
		 * Otherwise, if we don't have whitespace and have a bad token, it's an invalid
		 * delimiter
		 */
		case CP_CS0:
			switch(token)
			{
			case WHITESPACE:
				have_ws = last_column;
				break;
			case COLON:
				ERROR_AT_COLUMN(WEBVTT_MISSING_CUESETTING_KEYWORD,last_column);
				break;
			case VERTICAL: CHKDELIM have_ws = 0; SETST(CP_V1); break;
			case POSITION: CHKDELIM have_ws = 0; SETST(CP_P1); break;
			case ALIGN: CHKDELIM have_ws = 0; SETST(CP_A1); break;
			case SIZE: CHKDELIM have_ws = 0; SETST(CP_S1); break;
			case LINE: CHKDELIM have_ws = 0; SETST(CP_L1); break;
			default:
				if( have_ws )
				{
					ERROR_AT_COLUMN(WEBVTT_INVALID_CUESETTING,last_column);
					while( pos < len && buffer[pos] != 0x09 && buffer[pos] != 0x20 ) ++pos;	
				}
				else if( token == BADTOKEN )
				{
					/* it was a bad delimiter... */
					if( !baddelim )
					{
						baddelim = last_column;
					}
					++pos;
				}
			}
			break;
#define CS1(S) \
	if( token == COLON ) \
	{ if(have_ws) { ERROR_AT_COLUMN(WEBVTT_UNEXPECTED_WHITESPACE,have_ws); } SETST((S)); have_ws = 0; } \
	else if( token == WHITESPACE && !have_ws ) \
	{ \
		have_ws = last_column; \
	} \
	else \
	{ \
		switch(token) \
		{ \
		case LR: case RL: case INTEGER: case PERCENTAGE: case START: case MIDDLE: case END: case LEFT: case RIGHT: \
			 ERROR_AT_COLUMN(WEBVTT_MISSING_CUESETTING_DELIMITER,have_ws ? have_ws : last_column); break; \
		default: \
			ERROR_AT_COLUMN(WEBVTT_INVALID_CUESETTING_DELIMITER,last_column); \
			while( pos < len && buffer[pos] != 0x20 && buffer[pos] != 0x09 ) ++pos; \
			break; \
		} \
		have_ws = 0; \
	}

	/**
	 * If we get a COLON, we advance to the next state.
	 * If we encounter whitespace first, fire an "unexpected whitespace" error and continue.
	 * If we encounter a cue-setting value, fire a "missing cuesetting delimiter" error
	 * otherwise (eg vertical;rl), fire "invalid cuesetting delimiter" error
	 *
	 * this logic is performed by the CS1 macro, defined above
	 */
	case CP_V1: CS1(CP_V2); break;
	case CP_P1: CS1(CP_P2); break;
	case CP_A1: CS1(CP_A2); break;
	case CP_S1: CS1(CP_S2); break;
	case CP_L1: CS1(CP_L2); break;
#undef CS1

/* BV: emit the BAD_VALUE error for the appropriate setting, when required */
#define BV(T) \
ERROR_AT_COLUMN(WEBVTT_##T##_BAD_VALUE,last_column); \
while( pos < len && buffer[pos] != 0x20 && buffer[pos] != 0x09 ) ++pos; \
SETST(CP_CS0);

/* HV: emit the ALREADY_SET (have value) error for the appropriate setting, when required */
#define HV(T) \
if( cue->flags & CUE_HAVE_##T ) \
{ \
	ERROR_AT_COLUMN(WEBVTT_##T##_ALREADY_SET,last_column); \
}
/* WS: emit the WEBVTT_UNEXPECTED_WHITESPACE error when required. */
#define WS \
case WHITESPACE: \
	if( !have_ws ) \
	{ \
		ERROR_AT_COLUMN(WEBVTT_UNEXPECTED_WHITESPACE,last_column); \
		have_ws = last_column; \
	} \
break

/* set that the cue already has a value for this */
#define SV(T) cue->flags |= CUE_HAVE_##T
		case CP_V2:
			HV(VERTICAL);
			switch(token)
			{
			WS;
			case LR: cue->settings.vertical = WEBVTT_VERTICAL_LR; have_ws = 0; SETST(CP_CS0);
				SV(VERTICAL);
				break;
			case RL: cue->settings.vertical = WEBVTT_VERTICAL_RL; have_ws = 0; SETST(CP_CS0);
				SV(VERTICAL);
				break;
			default:
				BV(VERTICAL);
			}
			break;
		
		case CP_P2:
			HV(POSITION);
			switch(token)
			{
				WS;
				case PERCENTAGE:
					{
						int digits;
						const webvtt_byte *t = self->token;
						webvtt_int64 v = parse_int( &t, &digits );
						if( v < 0 )
						{
							BV(POSITION);
						}
						cue->settings.position = (webvtt_uint)v;
						SETST(CP_CS0);
						SV(POSITION);
					}
					break;
				default:
					BV(POSITION);
					break;
			}
			break;
		
		case CP_A2:
			HV(ALIGN);
			switch(token)
			{
			WS;
			case START: cue->settings.align = WEBVTT_ALIGN_START; have_ws = 0; SETST(CP_CS0); SV(ALIGN); break;
			case MIDDLE: cue->settings.align = WEBVTT_ALIGN_MIDDLE; have_ws = 0; SETST(CP_CS0); SV(ALIGN); break;
			case END: cue->settings.align = WEBVTT_ALIGN_END; have_ws = 0; SETST(CP_CS0); SV(ALIGN); break;
			case LEFT: cue->settings.align = WEBVTT_ALIGN_LEFT; have_ws = 0; SETST(CP_CS0); SV(ALIGN); break;
			case RIGHT: cue->settings.align = WEBVTT_ALIGN_RIGHT; have_ws = 0; SETST(CP_CS0); SV(ALIGN); break;
			default:
				BV(ALIGN);
				break;
			}
			break;
		
		case CP_S2:
			HV(SIZE);
			switch(token)
			{
				WS;
				case PERCENTAGE:
					{
						int digits;
						const webvtt_byte *t = self->token;
						webvtt_int64 v = parse_int( &t, &digits );
						if( v < 0 )
						{
							BV(SIZE);
						}
						cue->settings.size = (webvtt_uint)v;
						SETST(CP_CS0);
						SV(SIZE);
					}
					break;
				default:
					BV(SIZE);
					break;
			}
			break;

		case CP_L2:
			HV(LINE);
			switch(token)
			{
			WS;
			case INTEGER:
				{
					const webvtt_byte *t = self->token;
					webvtt_int64 v = parse_int( &t, &digits );
					cue->settings.line_is_relative = 0;
					cue->settings.line.no = (webvtt_int)v;
					SETST(CP_CS0);
					SV(LINE);
				}
				break;
			case PERCENTAGE:
				{
					const webvtt_byte *t = self->token;
					webvtt_int64 v = parse_int( &t, &digits );
					if( v < 0 )
					{
						BV(POSITION);
					}
					cue->settings.line.relative_position = (webvtt_uint)v;
					SETST(CP_CS0);
					SV(LINE);
				}
				break;
			default:
				BV(LINE);
				break;
			}
#undef BV
#undef HV
#undef SV
#undef WS
		}
		self->token_pos = 0;
		last_token = token;
	}
	/**
	 * If we didn't finish in a good state...
	 */
	if( state != CP_CS0 )
	{
		/* if we never made it to the cuesettings, we didn't finish the cuetimes */
		if( state < CP_CS0 )
		{
			ERROR(WEBVTT_UNFINISHED_CUETIMES);
			return -1;
		}
		else
		{
			/* if we did, we should report an error but continue parsing. */
			webvtt_error e = WEBVTT_INVALID_CUESETTING;
			switch(state)
			{
			case CP_V2: e = WEBVTT_VERTICAL_BAD_VALUE; break;
			case CP_P2: e = WEBVTT_POSITION_BAD_VALUE; break;
			case CP_A2: e = WEBVTT_ALIGN_BAD_VALUE; break;
			case CP_S2: e = WEBVTT_SIZE_BAD_VALUE; break;
			case CP_L2: e = WEBVTT_LINE_BAD_VALUE; break;
			}
			ERROR(e);
		}
	}
	else
	{
		if( baddelim )
		{
			ERROR_AT_COLUMN(WEBVTT_INVALID_CUESETTING_DELIMITER,baddelim);
		}
	}
#undef SETST
	return 0;
}

static WEBVTT_INTERN webvtt_status
parse_webvtt( webvtt_parser self, const webvtt_byte *buffer, webvtt_uint *ppos,
	webvtt_uint len, webvtt_uint *mode, int finish )
{
	webvtt_status status = WEBVTT_SUCCESS;
	webvtt_token token;
	webvtt_uint pos = *ppos;
	int settings_delimiter = 0;
	int skip_error = 0;
	int settings_whitespace = 0;
	webvtt_uint settings_whitespace_at;
	webvtt_uint missing_cuesetting_at;

	while( pos < len )
	{
		webvtt_uint last_column, last_line, last_pos;
		skip_error = 0;
_next:
		last_column = self->column;
		last_line = self->line;
		last_pos = pos;

		/**
		 * If we're in certain states, we don't want to get a token and just
		 * want to read text instead.
		 */
		if( SP->state == T_CUEREAD )
		{
			int v;
			webvtt_uint old_pos = pos;
			if( v = webvtt_bytearray_getline( &SP->v.text, buffer, &pos, 
					len, 0, finish ) )
			{
				if( v < 0 )
				{
					webvtt_delete_bytearray( &SP->v.text );
					SP->type = V_NONE;
					POP();
					ERROR(WEBVTT_ALLOCATION_FAILED);
					status = WEBVTT_OUT_OF_MEMORY;
					goto _finish;
				}
				/* POP the stack and let the previous frame deal with it */
				POP();
			}
		}

		/**
		 * Get the next token from the stream
		 *
		 * If the token is 'UNFINISHED', but we are at the end of our input
		 * data, change it to BADTOKEN because it will never be finished.
		 *
		 * Otherwise, if we are expecting further data at some point, and have
		 * an unfinished token, return and let the next chunk deal with it.
		 */
		if( SP->state != T_CUE || !(self->popped && FRAMEUP(1)->state == T_CUEREAD) )
		{
			/**
			 * We don't tokenize in certain states
			 */
			token = webvtt_lex( self, buffer, &pos, len, finish );
			if( token == UNFINISHED )
			{
				if( finish )
				{
					token = BADTOKEN;
				}
				else if( pos == len )
				{
					goto _finish;
				}
			}
		}
_recheck:
		switch( SP->state )
		{
		case T_INITIAL:
			/**
			 * In the initial state:
			 * We should have WEBVTT as the first token returned,
			 * otherwise this isn't really a valid file.
			 *
			 * If we get 'WEBVTT', push us into the TAG state, where we
			 * check for a tag comment (arbitrary text following a whitespace
			 * after the WEBVTT token) until a newline
			 *
			 * If WEBVTT is not the first token, then report error and
			 * abort parsing.
			 */
			if( token == WEBVTT )
			{
				PUSH0( T_TAG, 0, V_NONE );
				break;
			}
			else
			{
				if( pos != len )
				{
					if( !skip_error )
					{
						ERROR_AT_COLUMN(WEBVTT_MALFORMED_TAG,last_column);
						skip_error = 1;
					}
					status = WEBVTT_PARSE_ERROR;
					goto _finish;
				}
			}
			break;
			
		case T_TAG:
			/**
			 * If we have a WHITESPACE following the WEBVTT token,
			 * switch to T_TAGCOMMENT state and skip the comment.
			 * Otherwise, if it's a NEWLINE, we can just skip to the T_BODY
			 * state.
			 *
			 * Otherwise, we didn't actually have a WEBVTT token, and should
			 * feel ashamed.
			 */
			if( token == WHITESPACE )
			{
				/* switch to comment skipper */
				PUSH0( T_TAGCOMMENT, 0, V_NONE);
			}
			else if( token == NEWLINE )
			{
				/* switch to NEWLINE counter */
				POPBACK();
				self->popped = 0;
				SP->state = T_BODY;
				PUSH0(T_EOL,1,V_INTEGER);
				break;
			}
			else
			{
				/**
				 * This wasn't preceded by an actual WEBVTT token, it's more
				 * like WEBVTTasdasd, which is not valid. Report an error,
				 * which should be considered fatal.
				 */
				if( !skip_error )
				{
					ERROR_AT_COLUMN(WEBVTT_MALFORMED_TAG,FRAME(1)->column);
					skip_error = 1;
					status = WEBVTT_PARSE_ERROR;
					goto _finish;
				}
			}
			break;

		/**
		 * COMMENT -- Read until EOL, ignore everything else
		 */
		case T_TAGCOMMENT:
			switch(token)
			{
			case NEWLINE:
				/**
				 * If we encounter a newline, switch to NEWLINE mode,
				 * and set up so that when we POPBACK() we are in the
				 * T_BODY state.
				 */
				POPBACK();
				PUSH0(T_EOL,1,V_INTEGER);
				break;
			default:
				find_newline( buffer, &pos, len );
				continue;
			}
			break;

		case T_CUEID:
			switch(token)
			{
				/**
				 * We're only really expecting a newline here -- 
				 * The cue id should have been read already
				 */
				case NEWLINE:
					SP->state = T_FROM;
					break;
			}
			
		/**
		 * Count EOLs, POP when finished
		 */
		case T_EOL:
			switch(token)
			{
			case NEWLINE:
				SP->v.value++;
				break;
			default:
				POPBACK();
				RECHECK
			}
			break;

		case T_BODY:
			if( self->popped && FRAMEUP(1)->state == T_EOL )
			{ 
				if( FRAMEUP(1)->v.value < 2 )
				{
					ERROR_AT_COLUMN( WEBVTT_EXPECTED_EOL, 1 );
				}
				FRAMEUP(1)->state = 0;
				FRAMEUP(1)->v.value = 0;
			}
			if( token == NOTE )
			{
				PUSH0(T_COMMENT,0,V_NONE);
			}
			else if( token != NEWLINE )
			{
				webvtt_cue cue = 0;
				webvtt_bytearray tk = 0;
				if( WEBVTT_FAILED( status = webvtt_create_cue( &cue ) ) )
				{
					if( status == WEBVTT_OUT_OF_MEMORY )
					{
						ERROR( WEBVTT_ALLOCATION_FAILED );
					}
					goto _finish;
				}
				if( WEBVTT_FAILED( status = webvtt_create_bytearray_nt( 
					self->token, self->token_pos, &tk ) ) )
				{
					if( status == WEBVTT_OUT_OF_MEMORY )
					{
						ERROR( WEBVTT_ALLOCATION_FAILED );
					}
					webvtt_release_cue( &cue );
					goto _finish;
				}
				PUSH0( T_CUE, cue, V_CUE );
				PUSH0( T_CUEREAD, tk, V_TEXT );
			}
			break;


		case T_CUE:
			if( self->popped && FRAMEUP(1)->state == T_CUEREAD )
			{
				/**
				 * We're expecting either cue-id (contains '-->') or cue
				 * params
				 */
				webvtt_cue cue = SP->v.cue;
				webvtt_state *st = FRAMEUP(1);
				webvtt_bytearray text = st->v.text;
				static const webvtt_byte separator[] = { 
					ASCII_DASH, ASCII_DASH, ASCII_GT
				};

				st->type = V_NONE;
				st->v.value = 0;

				/**
				 * The type should be V_TEXT. If it's not, somethings wrong.
				 *
				 * TODO: Add debug assertion
				 */
				if( find_bytes( text->text, text->length, separator, 
					sizeof(separator) ) )
				{
					/* It's not a cue id, we found '-->'. It can't be a second
					   cueparams line, because if we had it, we would be in
					   a different state. */
					int v;
					/* backup the column */
					self->column = 1;
					if( ( v = parse_cueparams( self, text->text, 
						text->length, cue ) ) < 0 )
					{
						if( v == WEBVTT_PARSE_ERROR )
						{
							status = WEBVTT_PARSE_ERROR;
							goto _finish;
						}
						webvtt_delete_bytearray( &text );
						*mode = M_SKIP_CUE;
						goto _finish;
					}
					else
					{
						webvtt_delete_bytearray( &text );
						cue->flags |= CUE_HAVE_CUEPARAMS;
						*mode = M_CUETEXT;
						goto _finish;
					}
				}
				else
				{
					/* It is a cue-id */
					if( cue->flags & CUE_HAVE_ID )
					{
						/**
						 * This isn't actually a cue-id, because we already 
						 * have one. It seems to be cuetext, which is occurring
						 * before cue-params
						 */
						webvtt_delete_bytearray( &text );
						ERROR(WEBVTT_CUE_INCOMPLETE);
						*mode = M_SKIP_CUE;
						goto _finish;
					}
					else
					{
						webvtt_uint pos = 0;
						self->column += text->length;
						if( WEBVTT_FAILED( status = webvtt_string_append_utf8(
							&cue->id, text->text, &pos, text->length, 0 ) ) )
						{
							webvtt_delete_bytearray( &text );
							ERROR(WEBVTT_ALLOCATION_FAILED);
						}
						
						cue->flags |= CUE_HAVE_ID;
					}
				}
				webvtt_delete_bytearray( &text );
				self->popped = 0;
			}
			else
			{
				webvtt_cue cue = SP->v.cue;
				/* If we have a newline, it might be the end of the cue. */
				if( token == NEWLINE )
				{
					if( cue->flags & CUE_HAVE_CUEPARAMS )
					{
						*mode = M_CUETEXT;
					}
					else if( cue->flags & CUE_HAVE_ID )
					{
						PUSH0(T_CUEREAD,0,V_NONE);
					}
					else
					{
						/* I don't think this should ever happen? */
						POPBACK();
					}
				}
			}
			break;
		
		}

		/**
		 * reset token pos
		 */
		self->token_pos = 0;
	}


_finish:
	if( status == WEBVTT_OUT_OF_MEMORY )
	{
		cleanup_stack( self );
	}
	*ppos = pos;
	return status;
}


static WEBVTT_INTERN webvtt_status
parse_cuetext( webvtt_parser self, const webvtt_byte *buffer, webvtt_uint *ppos, webvtt_uint len, webvtt_uint *mode, int finish )
{
	/**
	 * TODO:
	 * Unify cuetext parsing
	 */
	return WEBVTT_PARSE_ERROR;
}

static WEBVTT_INTERN webvtt_status
read_cuetext( webvtt_parser self, const webvtt_byte *b, webvtt_uint *ppos, webvtt_uint len, webvtt_uint *mode, webvtt_bool finish )
{
	webvtt_status status = WEBVTT_SUCCESS;
	webvtt_uint pos = *ppos;
	int finished = 0;
	do
	{
		int v;
		if( ( v = webvtt_bytearray_getline( &self->line_buffer, b, &pos, len, &self->truncate, finish ) ) )
		{
			if( v < 0 )
			{
				status = WEBVTT_OUT_OF_MEMORY;
				goto _finish;
			}
			
			if( self->line_buffer->text[ self->line_buffer->length - 1 ] == ASCII_LF )
			{
				/**
				 * finished
				 */
				finished = 1;
			}
			webvtt_bytearray_putc( &self->line_buffer, ASCII_LF );

			if( pos < len )
			{
				if( b[pos] == ASCII_CR )
				{
					if( len - pos >= 2 && b[pos+1] == ASCII_LF )
					{
						++pos;
					}
					++pos;
				}
				else
				{
					++pos;
				}
			}
		}
	} while( pos < len && !finished );
_finish:
	*ppos = pos;
	return status;
}

WEBVTT_EXPORT webvtt_status
webvtt_parse_chunk( webvtt_parser self, const void *buffer, webvtt_uint len, webvtt_bool finished )
{
	webvtt_status status;
	webvtt_uint pos = 0;
	const webvtt_byte *b = (const webvtt_byte *)buffer;

	while( pos < len )
	{
		switch( self->mode )
		{
		case M_WEBVTT:
			if( WEBVTT_FAILED(status = parse_webvtt( self, b, &pos, len, &self->mode, finished )) )
			{
				return status;
			}
			break;

		case M_CUETEXT:
#if 0
			if( WEBVTT_FAILED(status = parse_cuetext( self, b, &pos, len, &self->mode, finished )) )
			{
				return status;
			}
#else
			/**
			 * read in cuetext
			 */
			if( WEBVTT_FAILED(status = read_cuetext( self, b, &pos, len, &self->mode, finished )) )
			{
				return status;
			}
#endif
			finish_cue( self, &SP->v.cue );
			SP->type = V_NONE;
			webvtt_delete_bytearray( &self->line_buffer );
			self->mode = M_WEBVTT;
			break;

		case M_SKIP_CUE:
			if( WEBVTT_FAILED(status = read_cuetext( self, b, &pos, len, &self->mode, finished )) )
			{
				return status;
			}
			webvtt_delete_bytearray( &self->line_buffer );
			self->mode = M_WEBVTT;
			break;

		case M_READ_LINE:
			{
				/**
				 * Read in a line of text into the line-buffer,
				 * we will and depending on our state, do something with it.
				 */
				int ret;
				if( ( ret = webvtt_bytearray_getline( &self->line_buffer, b, &pos, len, &self->truncate, finished ) ) )
				{
					static const webvtt_byte separator[] = { ASCII_DASH, ASCII_DASH, ASCII_GT };
					if( ret < 0 )
					{
						ERROR(WEBVTT_ALLOCATION_FAILED);
						return WEBVTT_OUT_OF_MEMORY;
					}
					self->mode = M_WEBVTT;
				}
				break;
			}
		}
	}

	if( finished )
	{
		/**
		 * If we've finished parsing, we should hand our cue over to the user, if we have one.
		 */
		if( SP->type == V_CUE )
		{
			if( !(SP->v.cue->flags & CUE_HAVE_CUEPARAMS) )
			{
				ERROR(WEBVTT_CUE_INCOMPLETE);
			}

			finish_cue( self, &SP->v.cue );
		}

		switch( SP->state )
		{
		case T_CUEID:
			ERROR(WEBVTT_CUE_INCOMPLETE);
			break;
		}
	}
	return WEBVTT_SUCCESS;
}

#undef SP
#undef AT_BOTTOM
#undef ON_HEAP
#undef STACK_SIZE
#undef FRAME
#undef PUSH
#undef POP

/**
 * Get an integer value from a series of digits.
 */
static webvtt_int64
parse_int( const webvtt_byte **pb, int *pdigits )
{
	int digits = 0;
	webvtt_int64 result = 0;
	webvtt_int64 mul = 1;
	const webvtt_byte *b = *pb;
	while( *b )
	{
		webvtt_byte ch = *b;
		if( ASCII_ISDIGIT(ch) )
		{
			/**
			 * Digit character, carry on
			 */
			result = result * 10 + ( ch - ASCII_0 );
			++digits;
		}
		else if( mul == 1 && digits == 0 && ch == ASCII_DASH )
		{
			mul = -1;
		}
		else
		{
			break;
		}
		++b;
	}
	*pb = b;
	if( pdigits )
	{
		*pdigits = digits;
	}
	return result * mul;
}

/**
 * Turn the token of a TIMESTAMP tag into something useful, and returns non-zero
 * returns 0 if it fails
 */
static int
parse_timestamp( webvtt_parser self, const webvtt_byte *b, webvtt_timestamp *result )
{
	webvtt_int64 tmp;
	int have_hours = 0;
	int digits;
	int malformed = 0;
	webvtt_int64 v[4];
    if (!ASCII_ISDIGIT(*b))
	{
		goto _malformed;
	}
	
	/* get sequence of digits */
	v[0] = parse_int( &b, &digits );

	/* assume v[0] contains hours if more or less than 2 digits, or value is greater than 59 */
	if (digits != 2 || v[0] > 59)
	{
		have_hours = 1;
	}
	
	/* fail if missing colon ':' character */
	if ( !*b || *b++ != ASCII_COLON)
	{
		malformed = 1;
	}
	
	/* fail if end of data reached, or byte is not an ASCII digit */
	if (!*b || !ASCII_ISDIGIT(*b))
	{
		malformed = 1;
	}
	
	/* get another integer value, fail if digits is not equal to 2 */
    v[1] = parse_int( &b, &digits );
	if( digits != 2 )
	{
		malformed = 1;
	}
	
	/* if we already know there's an hour component, or if the next byte is a colon ':',
	   read the next value */
	if (have_hours || (*b == ASCII_COLON))
	{
		if( *b++ != ASCII_COLON )
		{
			goto _malformed;
		}
        if( !*b || !ASCII_ISDIGIT(*b) )
		{
			malformed = 1;
		}
		v[2] = parse_int( &b, &digits );
		if( digits != 2 )
		{
			malformed = 1;
		}
    }
	else
	{
		/* Otherwise, if there is no hour component, shift everything over */
		v[2] = v[1];
		v[1] = v[0];
		v[0] = 0;
    }

	/* collect the manditory seconds-frac component. fail if there is no FULL_STOP '.'
	   or if there is no ascii digit following it */
	if( *b++ != ASCII_PERIOD || !ASCII_ISDIGIT(*b))
	{
		goto _malformed;
	}
	v[3] = parse_int( &b, &digits );
	if( digits != 3 )
	{
		malformed = 1;
	}
	
	/* Ensure that minutes and seconds are acceptable values */
	if( v[3] > 999 )
	{
#define MILLIS_PER_SEC (1000)
		tmp = v[3];
		v[2] += tmp / MILLIS_PER_SEC;
		v[3] = tmp % MILLIS_PER_SEC;
		malformed = 1;
	}
	if( v[2] > 59 )
	{
#define SEC_PER_MIN (60)
		tmp = v[2];
		v[1] += tmp / SEC_PER_MIN;
		v[2] = tmp % SEC_PER_MIN;
		malformed = 1;
	}
    if( v[1] > 59 )
	{
#define MIN_PER_HOUR (60)
		tmp = v[1];
		v[0] += tmp / MIN_PER_HOUR;
		v[1] = tmp % MIN_PER_HOUR;
		malformed = 1;
	}
	
	*result = (webvtt_timestamp)( v[0] * MSECS_PER_HOUR )
			+ ( v[1] * MSECS_PER_MINUTE )
			+ ( v[2] * MSECS_PER_SECOND )
			+ ( v[3] );
	
	if( malformed )
	{
		return 0;
	}
	return 1;
_malformed:
	*result = 0xFFFFFFFFFFFFFFFF;
	return 0;
}
