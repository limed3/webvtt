﻿#include "cue_testfixture"
class CueTimeTimestampHour : public CueTest { };

/***************************************************************************************/
/*                                     TIMESTAMP 1                                     */
/***************************************************************************************/

/**
 * Test expecting parser to fail when a 4 component timestamp's 'hours' component consists
 * of a non-digit
 * TIMESTAMP 1
 *
 * From http://dev.w3.org/html5/webvtt/#webvtt-timestamp (10/15/2012):
 * A WebVTT timestamp representing hours hours, minutes minutes, seconds seconds,
 * and thousandths of a second seconds-frac, consists of the following components,
 * in the given order:
 * 1. Optionally (required if hour is non-zero):
 *    a. Two or more characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT
 *       NINE (9), representing the hours as a base ten integer.
 *
 *    b. A U+003A COLON character (:)
 *
 * 2. Two characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9), 
 *    representing the minutes as a base ten integer in the range 0 ≤ minutes ≤ 59.
 *
 * 3. A U+003A COLON character (:)
 *
 * 4. Two characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9), representing the 
 *    seconds as a base ten integer in the range 0 ≤ seconds ≤ 59.
 *
 * 5. A U+002E FULL STOP character (.).
 *
 * 6. Three characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9), representing the 
 *    thousandths of a second seconds-frac as a base ten integer.
 */
TEST_F(CueTimeTimestampHour,DISABLED_NondigitTimestamp1)//C++ exception "std::bad_alloc"
{
  loadVtt( "cue-times/timestamp/timestamp1/hour/nondigit.vtt" );
  const Error& err = getError( 0 );
  ASSERT_EQ( WEBVTT_MALFORMED_TIMESTAMP, err.error() );
}

/***************************************************************************************/
/*                                     TIMESTAMP 2                                     */
/***************************************************************************************/

/**
 * Test expecting parser to fail when a 4 component timestamp's 'hours' component consists
 * of a non-digit
 * TIMESTAMP 2
 *
 * From http://dev.w3.org/html5/webvtt/#webvtt-timestamp (10/15/2012):
 * A WebVTT timestamp representing hours hours, minutes minutes, seconds seconds,
 * and thousandths of a second seconds-frac, consists of the following components,
 * in the given order:
 * 1. Optionally (required if hour is non-zero):
 *    a. Two or more characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT
 *       NINE (9), representing the hours as a base ten integer.
 *
 *    b. A U+003A COLON character (:)
 *
 * 2. Two characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9), 
 *    representing the minutes as a base ten integer in the range 0 ≤ minutes ≤ 59.
 *
 * 3. A U+003A COLON character (:)
 *
 * 4. Two characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9), representing the 
 *    seconds as a base ten integer in the range 0 ≤ seconds ≤ 59.
 *
 * 5. A U+002E FULL STOP character (.).
 *
 * 6. Three characters in the range U+0030 DIGIT ZERO (0) to U+0039 DIGIT NINE (9), representing the 
 *    thousandths of a second seconds-frac as a base ten integer.
 */
TEST_F(CueTimeTimestampHour,DISABLED_NondigitTimestamp2)//C++ exception "std::bad_alloc"
{
  loadVtt( "cue-times/timestamp/timestamp2/hour/nondigit.vtt" );
  const Error& err = getError( 0 );
  ASSERT_EQ( WEBVTT_MALFORMED_TIMESTAMP, err.error() );
}