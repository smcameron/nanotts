/* nanotts.cpp
 *
 * Copyright (C) 2014 Greg Naughton <greg@naughton.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *    Convert text to .wav using svox text-to-speech system.
 *    Rewrite of pico2wave.c
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h> // mmap

extern "C" {
#include "svoxpico/picoapi.h"
#include "svoxpico/picoapid.h"
#include "svoxpico/picoos.h"
}

#include "PicoVoices.h"
#include "mmfile.h"
#include "StreamHandler.h"

#ifdef _USE_ALSA
  #include "Player_Alsa.h"
#endif

#define PICO_DEFAULT_SPEED 0.88f
#define PICO_DEFAULT_PITCH 1.05f
#define PICO_DEFAULT_VOLUME 1.00f

/* string-ify a macro */
#define STRR(X) #X
#define STR(X) STRR(X)

#ifndef PICO_ROOT
#define PICO_ROOT /usr
#endif

// searching these paths
const char * lingware_paths[ 2 ] = { "./lang", STR(PICO_ROOT) "/share/pico/lang" };

#define FILE_OUTPUT_PREFIX "nanotts-output-"
#define FILE_OUTPUT_SUFFIX ".wav"
#define FILENAME_NUMBERING_LEADING_ZEROS 4

// software version information
#define CANONICAL_NAME      "nanotts"
#define CONFIG_DIR_NAME     ".nanotts"
#define VERSION_MAJOR       "0"
#define CON1                "."
#define VERSION_MINOR       "9"
#define CON2                "-"
// a = alpha, b = beta, rc = release-candidate, r = release
#define RELEASE_TYPE        "a"
#include "release_version.h" // RELEASE_VERSION, increments every build
#define SOFTWARE_VERSION VERSION_MAJOR CON1 VERSION_MINOR CON2 RELEASE_TYPE RELEASE_VERSION
#ifdef _USE_ALSA
  #define VERSIONED_NAME CANONICAL_NAME "-" SOFTWARE_VERSION "-alsa"
#else
  #define VERSIONED_NAME CANONICAL_NAME "-" SOFTWARE_VERSION
#endif


// forward declarations
int GetNextLowestFilenameNumber( const char * prefix, const char * suffix, int zeropad );
class Nano;

/*
================================================
Listener

stream class, for exchanging byte-streams between producer/consumer
================================================
*/
template <typename type>
class Listener {
    type * data;
    unsigned int read_p;
    void (Nano::*consume)( short *, unsigned int );
    Nano* nano_class;
public:
    Listener() : data(0), read_p(0), consume(0), nano_class(0) {
    }
    Listener(Nano *n) : data(0), read_p(0), consume(0), nano_class(n) {
    }
    virtual ~Listener() {
    }

    void writeData( type * data, unsigned int byte_size );
    void setCallback( void (Nano::*con_f)( short *, unsigned int ), Nano* =0 ) ;
    bool hasConsumer();
};

template <typename type>
void Listener<type>::writeData( type * data, unsigned int byte_size ) {
    if ( this->consume ) {
        void (Nano::*pointer)(short *, unsigned int) = this->consume;
        (*nano_class.*pointer)( data, byte_size );
    }
}

template <typename type>
void Listener<type>::setCallback( void (Nano::*con_f)( short *, unsigned int ), Nano* nano_class ) {
    this->consume = con_f;
    if ( nano_class )
        this->nano_class = nano_class;
}

template <typename type>
bool Listener<type>::hasConsumer() {
    return consume != 0;
}
//////////////////////////////////////////////////////////////////


struct pads_t {
    const char * name;
    const char * ofmt;
    const char * cfmt;
    float * val;
};

/*
================================================
Boilerplate

adds padding text around input to set various parameters in pico
================================================
*/
class Boilerplate {
    char plate_begin[100];
    char plate_end[50];

    float speed;
    float pitch;
    float volume;

    const unsigned int padslen;

    void setOne( const char * verb, float value ) {
        char buf[100];

        // find the parm and set it
        for ( unsigned int i = 0; i < padslen; i++ ) {
            if ( strcmp( pads[i].name, verb ) == 0 ) {
                *(pads[i].val) = value;
                break;
            }
        }

        // rebuild the plates
        memset( plate_begin, 0, 100 );
        memset( plate_end, 0, 50 );
        for ( unsigned int i = 0; i < padslen; i++ ) {
            if ( *(pads[i].val) != -1 ) {
                // begin plate
                int ivalue = ceilf( *(pads[i].val) * 100.0f );
                sprintf( buf, pads[i].ofmt, ivalue );
                strcat( plate_begin, buf );
                // end plate := reverse order to match tag order
                strcpy( buf, plate_end );
                sprintf( plate_end, "%s%s", pads[i].cfmt, buf );
                // </volume></pitch></speed>
            }
        }
    }

public:
    static pads_t pads[];

    Boilerplate() : padslen(3) {
        speed   = -1.0f;
        pitch   = -1.0f;
        volume  = -1.0f;

        memset( plate_begin, 0, 100 );
        memset( plate_end, 0, 50 );

        pads[0].val = &speed;
        pads[1].val = &pitch;
        pads[2].val = &volume;
    }

    Boilerplate( float s, float p, float v ) : padslen(3) {
        setSpeed( s );
        setPitch( p );
        setVolume( v );
    }

    bool isChanged() {
        return !((speed == -1.0f) && (pitch == -1.0f) && (volume == -1.0f));
    }

    char * getOpener(unsigned int * l) { *l = strlen(plate_begin); return plate_begin; }
    char * getCloser(unsigned int * l) { *l = strlen(plate_end); return plate_end; }

    void setSpeed( float f ) {
        setOne( "speed", f );
    }
    void setPitch( float f ) {
        setOne( "pitch", f );
    }
    void setVolume( float f ) {
        setOne( "volume", f );
    }

    const char * getStatusMessage() {
        static char buf[100];
        memset(buf, 0, 100);
        char tmp[40];

        for ( unsigned int i = 0; i < padslen; i++ ) {
            if ( *(pads[i].val) != -1 ) {
                pads_t * p = &pads[i];
                sprintf( tmp, "%s: %.2f\n", p->name, *p->val );
                strcat( buf, tmp );
            }
        }
        return buf;
    }
};

pads_t Boilerplate::pads[] = {
    {"speed",   "<speed level=\"%d\">",     "</speed>",     0},
    {"pitch",   "<pitch level=\"%d\">",     "</pitch>",     0},
    {"volume",  "<volume level=\"%d\">",    "</volume>",    0}
};



/*
================================================
    Nano

    class to handle workings of this program
================================================
*/
class Nano
{
private:
    enum inputMode_t {
        IN_NOT_SET,
        IN_STDIN,
        IN_CMDLINE_ARG,
        IN_CMDLINE_TRAILING,
        IN_SINGLE_FILE,
        IN_MULTIPLE_FILES
    };

    enum outputMode_t {
        OUT_NOT_SET         = 0,
        OUT_STDOUT          = 1,
        OUT_SINGLE_FILE     = 2,
        OUT_PLAYBACK        = 4,
        OUT_MULTIPLE_FILES  = 8
    };

    int                 in_mode;
    int                 out_mode;

    const int           my_argc;
    const char **       my_argv;
    const char *        exename;

    char *              voice;
    char *              langfiledir;
    char                prefix[ 256 ];
    char                suffix[ 100 ];
    char *              out_filename;
    char *              in_filename;
    char *              words;
    FILE *              in_fp;
    FILE *              out_fp;

    char *              copy_arg( int );

    unsigned char *     input_buffer;
    unsigned int        input_size;

    mmfile_t *          mmfile;

    Listener<short>     listener;
    void                write_short_to_stdout( short *, unsigned int );
    void                write_short_to_playback( short * data, unsigned int shorts );
    void                write_short_to_playback_and_stdout( short * data, unsigned int shorts );

    Boilerplate         modifiers;
    StreamHandler       streamHandler;

public:
    bool                silence_output;

    Nano( const int, const char ** );
    virtual ~Nano();

    void PrintUsage();
    int parse_commandline_arguments();
    int setup_input_output();
    int verify_input_output();

    int ProduceInput( unsigned char ** data, unsigned int * bytes );
    int playOutput();

    const char * getVoice();
    const char * getLangFilePath();

    const char * outFilename() const { return out_filename; }

    Listener<short> * getListener() ;

    Boilerplate * getModifiers() ;

    void SetListenerStdout();
    void SetListenerPlayback();
    void SetListenerPlaybackAndStdout();

    bool writingWaveFile() { return (out_mode & OUT_SINGLE_FILE)==OUT_SINGLE_FILE; }
};

Nano::Nano( const int i, const char ** v ) : my_argc(i), my_argv(v), listener(this) {
    voice = 0;
    langfiledir = 0;
    sprintf( prefix, FILE_OUTPUT_PREFIX );
    sprintf( suffix, FILE_OUTPUT_SUFFIX );
    out_filename = 0;
    in_filename = 0;
    words = 0;
    in_fp = 0;
    out_fp = 0;
    input_buffer = 0;
    input_size = 0;

    silence_output = true;
}

Nano::~Nano() {
    if ( voice )
        delete[] voice;
    if ( langfiledir )
        delete[] langfiledir;
    if ( out_filename )
        delete[] out_filename;
    if ( in_filename )
        delete[] in_filename;
    if ( words )
        delete[] words;

    if ( input_buffer ) {
        switch ( in_mode ) {
        case IN_STDIN:
            delete[] input_buffer;
            break;
        case IN_SINGLE_FILE:
            delete mmfile;
            mmfile = 0;
        default:
            break;
        }
        input_buffer = 0;
    }

    if ( in_fp != 0 && in_fp != stdin ) {
        fclose( in_fp );
        in_fp = 0;
    }
    if ( out_fp != 0 && out_fp != stdout ) {
        fclose( out_fp );
        out_fp = 0;
    }

}

void Nano::PrintUsage() {

    const char * program = strrchr( my_argv[0], '/' );
    program = !program ? my_argv[0] : program + 1;
    this->exename = program;

    printf( "usage: %s [options]\n", exename );

    char line1[ 100 ];
    char line2[ 100 ];
    char line3[ 100 ];
    char line4[ 100 ];
    memset( line1, 0, sizeof(line1) );
    memset( line2, 0, sizeof(line2) );
    memset( line3, 0, sizeof(line3) );
    memset( line4, 0, sizeof(line4) );
    sprintf( line1, "   %s -f ray_bradbury.txt -o ray_bradbury.wav", exename );
    sprintf( line2, "   echo \"Mary had a little lamb\" | %s --play", exename );
    sprintf( line3, "   %s -i \"Once upon a midnight dreary\" -v en-US --speed 0.8 --pitch 1.8 -w -p", exename );
    sprintf( line4, "   echo \"Brave Ulysses\" | %s -c | play -r 16k -L -t raw -e signed -b 16 -c 1 -", exename );

    struct help {
        const char *arg;
        const char *desc;
    } help_lines[] = {
        { "   -h, --help", "Displays this help. (overrides other input)" },
        { "   -v, --voice <voice>", "Select voice. (Default: en-GB)" },
        { "   -l <directory>", "Set Lingware voices directory. (defaults: \"./lang\", \"/usr/share/pico/lang/\")" },
        { "   -i <text>", "Input. (Text must be correctly quoted)" },
        { "   -f <filename>", "Filename to read input from" },
        { "   -o <filename>", "Write output to WAV/PCM file (enables WAV output)" },
        { "   -w, --wav ", "Write output to WAV file, will generate filename if '-o' option not provided" },
        { "   -p, --play ", "Play audio output" },
        { "   -m, --no-play", "do NOT play output on PC's soundcard" },
        { "   -c ", "Send raw PCM output to stdout" },
        { "   --prefix", "Set the file prefix (eg. \"MyRecording-\")." },
        { "", "Generated files will be auto-numbered." },
        { "", "Good for running multiple times with different inputs" },
        { "   --speed <0.2-5.0>", "change voice speed" },
        { "   --pitch <0.5-2.0>", "change voice pitch" },
        { "   --volume <0.0-5.0>", "change voice volume (>1.0 may result in degraded quality)" },
        { "   --version", "Displays version information about this program" },
//        { "  --files", "set multiple input files" },
        { " ", " " },
        { "Possible Voices: ", " " },
        { "   en-US, en-GB, de-DE, es-ES, fr-FR, it-IT", " " },
        { " ", " " },
        { "Examples: ", " " },
        { line1, " " },
        { line2, " " },
        { line3, " " },
        { line4, " " },
        { " ", " " },
    };

    unsigned long long int size = *(&help_lines + 1) - help_lines;

    for ( unsigned int i = 0; i < size; i++ ) {
        printf( "%-24s%s\n", help_lines[i].arg, help_lines[i].desc );
    }
}

// get argument at index, make a copy and return it
char * Nano::copy_arg( int index )
{
    if ( index >= my_argc )
        return 0;

    int len = strlen( my_argv[index] );
    char * buf = new char[len+1];
    memset( buf, 0, len + 1 );
    strcpy( buf, my_argv[index] );
    return buf;
}

int Nano::parse_commandline_arguments()
{
    // inputs and especially output are explicit
    in_mode = IN_NOT_SET;
    out_mode = OUT_NOT_SET;
    bool trailing_args = false;

#define WARN_UNMATCHED_INPUTS() do{     \
    if (trailing_args) {                \
        fprintf(stderr," **warning: commandline switch: '%s' in trailing inputs\n",my_argv[i]);  \
        break;                          \
    }                                   \
}while(0)

    if ( ! isatty(fileno(stdin)) ) {
        in_mode = IN_STDIN;
    }

    for ( int i = 1; i < my_argc; i++ )
    {
        // PRINT HELP
        if ( strcmp( my_argv[i], "-h" ) == 0 || strcmp( my_argv[i], "--help" ) == 0 ) {
            return -1;
        }
        if ( strcmp( my_argv[i], "--version" ) == 0 ) {
            fprintf( stderr, "%s\n", VERSIONED_NAME );
            return -666;
        }

        // INPUTS
        else if ( strcmp( my_argv[i], "-i" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( in_mode != IN_NOT_SET ) {
                fprintf( stderr, " **error: multiple inputs\n\n" );
                return -1;
            }
            in_mode = IN_CMDLINE_ARG;
            if ( (words = copy_arg( i + 1 )) == 0 )
                return -1;
            ++i;
        }
        else if ( strcmp( my_argv[i], "-f" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( in_mode != IN_NOT_SET ) {
                fprintf( stderr, " **error: multiple inputs\n\n" );
                return -1;
            }
            in_mode = IN_SINGLE_FILE;
            if ( (in_filename = copy_arg( i + 1 )) == 0 )
                return -1;
            ++i;
        }
        else if ( strcmp( my_argv[i], "--files" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( in_mode != IN_NOT_SET ) {
                fprintf( stderr, " **error: multiple inputs\n\n" );
                return -1;
            }
            in_mode = IN_MULTIPLE_FILES;
            // FIXME: get array of char*filename
            if ( (in_filename = copy_arg( i + 1 )) == 0 )
                return -1;
            ++i;
        }
        else if ( strcmp( my_argv[i], "-" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( in_mode != IN_NOT_SET && in_mode != IN_STDIN ) {
                fprintf( stderr, " **error: multiple inputs\n\n" );
                return -1;
            }
            in_mode = IN_STDIN;
            in_fp = stdin;
        }

        // OUTPUTS
        else if ( strcmp( my_argv[i], "-o" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            out_mode |= OUT_SINGLE_FILE;
            if ( (out_filename = copy_arg( i + 1 )) == 0 )
                return -1;
            ++i;
        }
        else if ( strcmp( my_argv[i], "-c" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            out_mode |= OUT_STDOUT;
        }
        else if ( strcmp( my_argv[i], "-w" ) == 0 || strcmp( my_argv[i], "--wav" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            out_mode |= OUT_SINGLE_FILE;
        }
        else if ( strcmp( my_argv[i], "-m" ) == 0 || strcmp( my_argv[i], "--no-play" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            silence_output = true;
            out_mode &= ~OUT_PLAYBACK;
        }
        else if ( strcmp( my_argv[i], "-p" ) == 0 || strcmp( my_argv[i], "--play" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            silence_output = false;
            out_mode |= OUT_PLAYBACK;
        }
        else if ( strcmp( my_argv[i], "--prefix" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            out_mode |= OUT_SINGLE_FILE;
            if ( i + 1 >= my_argc )
                return -1;
            strncpy( prefix, my_argv[i+1], sizeof(prefix) );
            ++i;
        }

        // SVOX
        else if ( strcmp( my_argv[i], "-v" ) == 0 || strcmp( my_argv[i], "--voice" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( (voice = copy_arg( i + 1 )) == 0 )
                return -1;
            ++i;
        }
        else if ( strcmp( my_argv[i], "-l" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( (langfiledir = copy_arg( i + 1 )) == 0 )
                return -1;
            fprintf( stderr, "Using Lingware directory: %s\n", langfiledir );
            ++i;
        }

        // OTHER
        else if ( strcmp( my_argv[i], "--speed" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( i + 1 >= my_argc )
                return -1;
            modifiers.setSpeed( strtof(my_argv[i+1], 0) );
            ++i;
        }
        else if ( strcmp( my_argv[i], "--pitch" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( i + 1 >= my_argc )
                return -1;
            modifiers.setPitch( strtof(my_argv[i+1], 0) );
            ++i;
        }
        else if ( strcmp( my_argv[i], "--volume" ) == 0 ) {
            WARN_UNMATCHED_INPUTS();
            if ( i + 1 >= my_argc )
                return -1;
            modifiers.setVolume( strtof(my_argv[i+1], 0) );
            ++i;
        }

        // doesn't match any expected arguments; therefor try to speak it
        else {
            if ( in_mode != IN_NOT_SET && in_mode != IN_CMDLINE_TRAILING ) {
                fprintf( stderr, " **error: trailing commandline arguments\n\n" );
                return -4;
            }

            // TODO: collect the valid straggling terms and concat them together
            trailing_args = true; // behavior changes once we encounter trailing arguments
            words = copy_arg( i );
            in_mode = IN_CMDLINE_TRAILING;
        }
    }

    if ( verify_input_output() < 0 ) {
        return -3;
    }

    // DEFAULTS
    if ( !voice ) {
        voice = new char[6];
        strcpy( voice, "en-GB" );
    }

    if ( !langfiledir ) {
        const char * path_p = 0;
        char test_file[ 128 ];
        for ( unsigned int i = 0; i < sizeof(lingware_paths)/sizeof(lingware_paths[0]); ++i ) {
            sprintf( test_file, "%s/%s", lingware_paths[i], "en-GB_ta.bin" );
            struct stat ss;
            if ( -1 != stat( lingware_paths[i], &ss ) ) {
                if ( S_ISDIR( ss.st_mode ) ) {
                    if ( -1 != stat( test_file, &ss ) ) {
                        if ( S_ISREG( ss.st_mode ) ) {
                            path_p = lingware_paths[i];
                            break;
                        }
                    }
                }
            }
        }

        if ( !path_p ) {
            fprintf( stderr, " **error: Lang file path not found. Looking in: %s, %s\n\n", lingware_paths[0], lingware_paths[1] );
            return -8;
        }

        int len = strlen( path_p ) + 1;
        langfiledir = new char[len];
        strcpy( langfiledir, path_p );
    }

    if ( !out_filename ) {
        int tmpsize = 100;
        out_filename = new char[ tmpsize ];
        memset( out_filename, 0, tmpsize );
        int next = GetNextLowestFilenameNumber( prefix, suffix, FILENAME_NUMBERING_LEADING_ZEROS );
        char fmt[ 32 ];
        sprintf( fmt, "%%s%%0%dd%%s", FILENAME_NUMBERING_LEADING_ZEROS );
        sprintf( out_filename, fmt, prefix, next, suffix );
    }

    //
    if ( setup_input_output() < 0 ) {
        return -2;
    }

    return 0;
}

int Nano::setup_input_output()
{
#define __NOT_IMPL__ do{fprintf(stderr," ** not implemented ** \n");return -1;}while(0);

    switch ( in_mode ) {
    case IN_STDIN:
        if ( in_fp )
            break;
        // detect if stdin is coming from a pipe
        if ( ! isatty(fileno(stdin)) ) { // On windows prefix with underscores: _isatty, _fileno
            in_fp = stdin;
        } else {
            if ( !words ) {
                fprintf( stderr, " **error: reading from stdin.\n\n" );
                return -1;
            }
        }
        break;
    case IN_SINGLE_FILE:
        // mmap elsewhere
        break;
    case IN_CMDLINE_ARG:
    case IN_CMDLINE_TRAILING:
        break;
    case IN_MULTIPLE_FILES:
        __NOT_IMPL__
    default:
        __NOT_IMPL__
        break;
    };

    int modes[] = { OUT_STDOUT, OUT_SINGLE_FILE, OUT_PLAYBACK, OUT_MULTIPLE_FILES };
    for ( int i = 0; i < 4; ++i ) {
        int test_mode = modes[i] & out_mode;
        switch ( test_mode ) {
        case OUT_SINGLE_FILE:
            break;
        case OUT_PLAYBACK:
            break;
        case OUT_STDOUT:
            out_fp = stdout;
            fprintf( stderr, "writing pcm stream to stdout\n" );
            break;
        case OUT_MULTIPLE_FILES:
            __NOT_IMPL__
            break;
        default:
            break;
        }
    }

    if ( (out_mode & (OUT_PLAYBACK|OUT_STDOUT)) == (OUT_PLAYBACK|OUT_STDOUT) ) {
        SetListenerPlaybackAndStdout();
    } else if ( out_mode & OUT_PLAYBACK ) {
        SetListenerPlayback();
    } else if ( out_mode & OUT_STDOUT ) {
        SetListenerStdout();
    }

#undef __NOT_IMPL__
    return 0;
}

int Nano::verify_input_output() {
    if ( in_mode == IN_NOT_SET ) {
        fprintf( stderr, " **error: no input\n\n" );
        return -1;
    }

    if ( out_mode == OUT_NOT_SET ) {
        fprintf( stderr, " **error: no output mode selected\n\n" );
        return -2;
    }

    return 0;
}

void Nano::SetListenerStdout() {
    listener.setCallback( &Nano::write_short_to_stdout );
}
void Nano::SetListenerPlayback() {
#ifdef _USE_ALSA
    streamHandler.player = new Player_Alsa();
#endif
    streamHandler.StreamOpen();
    listener.setCallback( &Nano::write_short_to_playback );
}
void Nano::SetListenerPlaybackAndStdout() {
#ifdef _USE_ALSA
    streamHandler.player = new Player_Alsa();
#endif
    streamHandler.StreamOpen();
    listener.setCallback( &Nano::write_short_to_playback_and_stdout );
}

// puts input into *data, and number_bytes into bytes
// returns 0 on no more data
int Nano::ProduceInput( unsigned char ** data, unsigned int * bytes )
{
    switch( in_mode ) {
    case IN_STDIN:
        input_buffer = new unsigned char[ 1000000 ];
        memset( input_buffer, 0, 1000000 );
        input_size = fread( input_buffer, 1, 1000000, stdin );
        *data = input_buffer;
        *bytes = input_size + 1;    /* pico expects 1 for terminating '\0' */
        fprintf( stderr, "read: %u bytes from stdin\n", input_size );
        break;
    case IN_SINGLE_FILE:
        mmfile = new mmfile_t( in_filename );
        *data = mmfile->data;
        *bytes = mmfile->size + 1;  /* 1 additional for terminating '\0' */
        fprintf( stderr, "read: %u bytes from \"%s\"\n", mmfile->size, in_filename );
        break;
    case IN_CMDLINE_ARG:
    case IN_CMDLINE_TRAILING:
        *data = (unsigned char *)words;
        *bytes = strlen(words) + 1; /* 1 additional for terminating '\0' */
        fprintf( stderr, "read: %u bytes from command line\n", *bytes );
        break;
    case IN_MULTIPLE_FILES:
        fprintf( stderr, "multiple files not supported\n" );
        return -1;
    default:
        fprintf( stderr, "unknown input\n" );
        return -1;
    }

    return 0;
}

//
int Nano::playOutput()
{
    if ( silence_output )
        return 0;

    return 1;
}

const char * Nano::getVoice() {
    return voice;
}

const char * Nano::getLangFilePath() {
    return langfiledir;
}

void Nano::write_short_to_stdout( short * data, unsigned int shorts ) {
    if ( out_mode & OUT_STDOUT )
        fwrite( data, 2, shorts, out_fp );
}

void Nano::write_short_to_playback( short * data, unsigned int shorts ) {
    if ( out_mode & OUT_PLAYBACK )
        streamHandler.SubmitFrames( (unsigned char *)data, shorts );
}

void Nano::write_short_to_playback_and_stdout( short * data, unsigned int shorts ) {
    if ( out_mode & OUT_STDOUT )
        fwrite( data, 2, shorts, out_fp );
    if ( out_mode & OUT_PLAYBACK )
        streamHandler.SubmitFrames( (unsigned char*)data, shorts );
}

Listener<short> * Nano::getListener() {
    if ( !listener.hasConsumer() )
        return 0;
    return &listener;
}

Boilerplate * Nano::getModifiers() {
    if ( modifiers.isChanged() )
        return &modifiers;
    return 0;
}
//////////////////////////////////////////////////////////////////


/*
================================================
    NanoSingleton

    singleton subclass
================================================
*/
class NanoSingleton : public Nano {
public:

    /**
     * only method to get singleton handle to this object
     */
    static NanoSingleton& instance();

    /**
     * explicit destruction
     */
    static void destroy();

    /**
     * utility method to pass-through arguments
     */
    static void setArgs( const int i, const char ** v );

private:
    /**
     * prevent outside construction
     */
    NanoSingleton( const int, const char ** );

    /**
     * prevent compile-time deletion
     */
    virtual ~NanoSingleton();

    /**
     * prevent copy-constructor
     */
    NanoSingleton( const NanoSingleton& ) ;

    /**
     * prevent assignment operator
     */
    NanoSingleton& operator=( const NanoSingleton & ) ;

    /**
     * sole instance of this object; static assures 0 initialization for free
     */
    static NanoSingleton * single_instance;
    static int              my_argc;
    static const char **    my_argv;
};

NanoSingleton * NanoSingleton::single_instance;
int             NanoSingleton::my_argc;
const char **   NanoSingleton::my_argv;

// implementations
NanoSingleton & NanoSingleton::instance() {
    if ( 0 == single_instance ) {
        single_instance = new NanoSingleton( my_argc, my_argv );
    }
    return *single_instance;
}

void NanoSingleton::destroy() {
    if ( single_instance ) {
        delete single_instance;
        single_instance = 0;
    }
}

NanoSingleton::NanoSingleton( const int i, const char ** v ) : Nano( i, v ) {
}

NanoSingleton::~NanoSingleton() {
}

void NanoSingleton::setArgs( const int i, const char ** v ) {
    my_argc = i;
    my_argv = v;
}
//////////////////////////////////////////////////////////////////



/*
================================================
Pico

class to encapsulate the workings of the SVox PicoTTS System
================================================
*/
class Pico {
private:
    PicoVoices_t        voices;

    pico_System         picoSystem;
    pico_Resource       picoTaResource;
    pico_Resource       picoSgResource;
    pico_Engine         picoEngine;
    picoos_SDFile       sdOutFile;
    char *              out_filename;

    pico_Char *         local_text;
    pico_Int16          text_remaining;
    long long int       total_text_length;
    char *              picoLingwarePath;

    char                picoVoiceName[10];
    Listener<short> *   listener;
    Boilerplate *       modifiers;

    void *              picoMemArea;
    pico_Char *         picoTaFileName;
    pico_Char *         picoSgFileName;
    pico_Char *         picoTaResourceName;
    pico_Char *         picoSgResourceName;
    bool                pico_writeWavPcm;

public:
    Pico() ;
    virtual ~Pico() ;

    void setLangFilePath( const char * path =0 );
    int initializeSystem() ;
    void cleanup() ;
    void sendTextForProcessing( unsigned char *, long long int ) ;
    int process();

    int setVoice( const char * );
    void setOutFilename( const char * fn ) { out_filename = const_cast<char*>(fn); }

    int fileSize( const char * filename ) ;
    void setListener( Listener<short> * );
    void addModifiers( Boilerplate * );
    void writeWavePcm( bool new_setting = true ) { pico_writeWavPcm = new_setting; }
};


Pico::Pico() {
    picoSystem              = 0;
    picoTaResource          = 0;
    picoSgResource          = 0;
    picoEngine              = 0;
    sdOutFile               = 0;
    picoLingwarePath        = 0;
    out_filename            = 0;

    strcpy( picoVoiceName, "PicoVoice" );

    total_text_length       = 0;
    text_remaining          = 0;
    listener                = 0;
    modifiers               = 0;

    picoMemArea             = 0;
    picoTaFileName          = 0;
    picoSgFileName          = 0;
    picoTaResourceName      = 0;
    picoSgResourceName      = 0;

    pico_writeWavPcm        = false;
}

Pico::~Pico() {
    if ( picoLingwarePath ) {
        delete[] picoLingwarePath;
    }

    cleanup();

    if ( picoMemArea )
        free( picoMemArea );
    if ( picoTaFileName )
        free( picoTaFileName );
    if ( picoSgFileName )
        free( picoSgFileName );
    if ( picoTaResourceName )
        free( picoTaResourceName );
    if ( picoSgResourceName )
        free( picoSgResourceName );
}

void Pico::setLangFilePath( const char * path ) {
    unsigned int len = strlen( path ) + 1;
    picoLingwarePath = new char[ len ];
    strcpy( picoLingwarePath, path );
}

int Pico::initializeSystem()
{
    const int       PICO_MEM_SIZE           = 2500000;
    pico_Retstring  outMessage;
    int             ret;

    picoMemArea = malloc( PICO_MEM_SIZE );

    if ( (ret = pico_initialize( picoMemArea, PICO_MEM_SIZE, &picoSystem )) ) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot initialize pico (%i): %s\n", ret, outMessage );

        pico_terminate(&picoSystem);
        picoSystem = 0;
        return -1;
    }

    /* Load the text analysis Lingware resource file.   */
    picoTaFileName = (pico_Char *) malloc( PICO_MAX_DATAPATH_NAME_SIZE + PICO_MAX_FILE_NAME_SIZE );

    // path
    if ( !picoLingwarePath )
        setLangFilePath();
    strcpy((char *) picoTaFileName, picoLingwarePath);

    // check for connecting slash
    unsigned int len = strlen( (const char*)picoTaFileName );
    if ( picoTaFileName[len-1] != '/' )
        strcat((char*) picoTaFileName, "/");

    // langfile name
    strcat( (char *) picoTaFileName, voices.getTaName() );

    // attempt to load it
    if ( (ret = pico_loadResource(picoSystem, picoTaFileName, &picoTaResource)) ) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot load text analysis resource file (%i): %s\n", ret, outMessage );
        goto unloadTaResource;
    }

    /* Load the signal generation Lingware resource file.   */
    picoSgFileName = (pico_Char *) malloc( PICO_MAX_DATAPATH_NAME_SIZE + PICO_MAX_FILE_NAME_SIZE );

    strcpy((char *) picoSgFileName,   picoLingwarePath );
    if ( picoSgFileName[len-1] != '/' )
        strcat((char*) picoSgFileName, "/");
    strcat((char *) picoSgFileName,   voices.getSgName() );

    if ( (ret = pico_loadResource(picoSystem, picoSgFileName, &picoSgResource)) ) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot load signal generation Lingware resource file (%i): %s\n", ret, outMessage );
        goto unloadSgResource;
    }

    /* Get the text analysis resource name.     */
    picoTaResourceName = (pico_Char *) malloc( PICO_MAX_RESOURCE_NAME_SIZE );
    if((ret = pico_getResourceName( picoSystem, picoTaResource, (char *) picoTaResourceName ))) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot get the text analysis resource name (%i): %s\n", ret, outMessage );
        goto unloadSgResource;
    }

    /* Get the signal generation resource name. */
    picoSgResourceName = (pico_Char *) malloc( PICO_MAX_RESOURCE_NAME_SIZE );
    if((ret = pico_getResourceName( picoSystem, picoSgResource, (char *) picoSgResourceName ))) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot get the signal generation resource name (%i): %s\n", ret, outMessage );
        goto unloadSgResource;
    }

    /* Create a voice definition.   */
    if((ret = pico_createVoiceDefinition( picoSystem, (const pico_Char *) picoVoiceName ))) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot create voice definition (%i): %s\n", ret, outMessage );
        goto unloadSgResource;
    }

    /* Add the text analysis resource to the voice. */
    if((ret = pico_addResourceToVoiceDefinition( picoSystem, (const pico_Char *) picoVoiceName, picoTaResourceName ))) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot add the text analysis resource to the voice (%i): %s\n", ret, outMessage );
        goto unloadSgResource;
    }

    /* Add the signal generation resource to the voice. */
    if((ret = pico_addResourceToVoiceDefinition( picoSystem, (const pico_Char *) picoVoiceName, picoSgResourceName ))) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot add the signal generation resource to the voice (%i): %s\n", ret, outMessage );
        goto unloadSgResource;
    }

    /* Create a new Pico engine. */
    if((ret = pico_newEngine( picoSystem, (const pico_Char *) picoVoiceName, &picoEngine ))) {
        pico_getSystemStatusMessage(picoSystem, ret, outMessage);
        fprintf( stderr, "Cannot create a new pico engine (%i): %s\n", ret, outMessage );
        goto disposeEngine;
    }

    /* success */
    return 0;


    //
    // partial shutdowns below this line
    //  for pico cleanup in case of startup abort
    //
disposeEngine:
    if (picoEngine) {
        pico_disposeEngine( picoSystem, &picoEngine );
        pico_releaseVoiceDefinition( picoSystem, (pico_Char *) picoVoiceName );
        picoEngine = 0;
    }
unloadSgResource:
    if (picoSgResource) {
        pico_unloadResource( picoSystem, &picoSgResource );
        picoSgResource = 0;
    }
unloadTaResource:
    if (picoTaResource) {
        pico_unloadResource( picoSystem, &picoTaResource );
        picoTaResource = 0;
    }

    if (picoSystem) {
        pico_terminate(&picoSystem);
        picoSystem = 0;
    }

    return -1;
}

void Pico::cleanup()
{
    if ( sdOutFile ) {
        picoos_Common common = (picoos_Common) pico_sysGetCommon(picoSystem);
        picoos_sdfCloseOut(common, &sdOutFile);
        sdOutFile = 0;
    }

    if (picoEngine) {
        pico_disposeEngine( picoSystem, &picoEngine );
        pico_releaseVoiceDefinition( picoSystem, (pico_Char *) picoVoiceName );
        picoEngine = 0;
    }

    if (picoSgResource) {
        pico_unloadResource( picoSystem, &picoSgResource );
        picoSgResource = 0;
    }

    if (picoTaResource) {
        pico_unloadResource( picoSystem, &picoTaResource );
        picoTaResource = 0;
    }

    if (picoSystem) {
        pico_terminate(&picoSystem);
        picoSystem = 0;
    }
}

void Pico::sendTextForProcessing( unsigned char * words, long long int word_len )
{
    local_text          = (pico_Char *) words;
    total_text_length   = word_len;
}

int Pico::process()
{
    const int       MAX_OUTBUF_SIZE     = 128;
    const int       PCM_BUFFER_SIZE     = 256;
    pico_Char *     inp                 = 0;
    pico_Int16      bytes_sent, bytes_recv, out_data_type;
    short           outbuf[MAX_OUTBUF_SIZE/2];
    pico_Retstring  outMessage;
    char            pcm_buffer[ PCM_BUFFER_SIZE ];
    int             ret, getstatus;
    picoos_bool     done                = TRUE;

    bool            do_startpad         = false;
    bool            do_endpad           = false;

    // pads are optional, but can be provided to set pico-modifiers
    if ( modifiers ) {
        do_startpad = true;
        do_endpad = true;
        unsigned int len;
        inp = (pico_Char *) modifiers->getOpener( &len );
        text_remaining = len;
        fprintf( stderr, "%s", modifiers->getStatusMessage() );
    } else {
        inp = (pico_Char *) local_text;
    }

    unsigned int bufused = 0;
    memset( pcm_buffer, 0, PCM_BUFFER_SIZE );

    // open output WAVE/PCM for writing
    if ( pico_writeWavPcm ) {
        picoos_Common common = (picoos_Common) pico_sysGetCommon(picoSystem);
        if ( TRUE != (done=picoos_sdfOpenOut(common, &sdOutFile, (picoos_char *)out_filename, SAMPLE_FREQ_16KHZ, PICOOS_ENC_LIN)) ) {
            fprintf( stderr, "Cannot open output wave file: %s\n", out_filename );
            return -1;
        }
    }

    long long int text_length = total_text_length;

    /* synthesis loop   */
    while(1)
    {
        //−32,768 to 32,767.
        if (text_remaining <= 0)
        {
            // text_remaining run-out; end pre-pad text
            if ( do_startpad ) {
                do_startpad = false;
                // start normal text
                inp = (pico_Char *) local_text;
                int increment = text_length >= 32767 ? 32767 : text_length;
                text_length -= increment;
                text_remaining = increment;
            }
            // main text ran out
            else if ( text_length <= 0 ) {
                // tack end_pad on the end
                if ( do_endpad ) {
                    do_endpad = false;
                    unsigned int len;
                    inp = (pico_Char *) modifiers->getCloser( &len );
                    text_remaining = len;
                } else {
                    break; /* done */
                }
            }
            // continue feed main text
            else {
                int increment = text_length >= 32767 ? 32767 : text_length;
                text_length -= increment;
                text_remaining = increment;
            }
        }

        /* Feed the text into the engine.   */
        if ( (ret = pico_putTextUtf8(picoEngine, inp, text_remaining, &bytes_sent)) ) {
            pico_getSystemStatusMessage(picoSystem, ret, outMessage);
            fprintf( stderr, "Cannot put Text (%i): %s\n", ret, outMessage );
            return -2;
        }

        text_remaining -= bytes_sent;
        inp += bytes_sent;

        do {

            /* Retrieve the samples */
            getstatus = pico_getData( picoEngine, (void *) outbuf, MAX_OUTBUF_SIZE, &bytes_recv, &out_data_type );
            if ( (getstatus !=PICO_STEP_BUSY) && (getstatus !=PICO_STEP_IDLE) ) {
                pico_getSystemStatusMessage(picoSystem, getstatus, outMessage);
                fprintf( stderr, "Cannot get Data (%i): %s\n", getstatus, outMessage );
                return -4;
            }

            /* copy partial encoding and get more bytes */
            if ( bytes_recv > 0 )
            {
                if ( (bufused + bytes_recv) <= PCM_BUFFER_SIZE ) {
                    memcpy( pcm_buffer+bufused, (int8_t *)outbuf, bytes_recv );
                    bufused += bytes_recv;
                }

                /* or write the buffer to wavefile, and retrieve any leftover decoding bytes */
                else
                {
                    if ( pico_writeWavPcm ) {
                        done = picoos_sdfPutSamples( sdOutFile, bufused / 2, (picoos_int16*) pcm_buffer );
                    }

                    if ( listener ) {
                        listener->writeData( (short*)pcm_buffer, bufused/2 );
                    }

                    bufused = 0;
                    memcpy( pcm_buffer, (int8_t *)outbuf, bytes_recv );
                    bufused += bytes_recv;
                }
            }

        } while (PICO_STEP_BUSY == getstatus);

        /* This chunk of synthesis is finished; pass the remaining samples. */
        if ( pico_writeWavPcm ) {
            done = picoos_sdfPutSamples( sdOutFile, bufused / 2, (picoos_int16*) pcm_buffer );
        }

        if ( listener ) {
            listener->writeData( (short*)pcm_buffer, bufused/2 );
        }
    }

    // close output wave file, so it can be opened elsewhere
    if ( sdOutFile ) {
        picoos_Common common = (picoos_Common) pico_sysGetCommon(picoSystem);
        picoos_sdfCloseOut(common, &sdOutFile);
        sdOutFile = 0;

        // report
        int bytes = fileSize( out_filename );
        fprintf( stderr, "wrote \"%s\" (%d bytes)\n", out_filename, bytes );
    }

    return bufused;
}

int Pico::setVoice( const char * v ) {
    int r = voices.setVoice( v ) ;
    fprintf( stderr, "using lang: %s\n", voices.getVoice() );
    return r;
}

int Pico::fileSize( const char * filename ) {
    FILE * file_p = fopen( filename, "rb" );
    if ( !file_p )
        return -1;
    fseek( file_p, 0L, SEEK_SET );
    int beginning = ftell( file_p );
    fseek( file_p, 0L, SEEK_END );
    int end = ftell( file_p );
    return end - beginning;
}

void Pico::setListener( Listener<short> * listener ) {
    this->listener = listener;
}

void Pico::addModifiers( Boilerplate * modifiers ) {
    this->modifiers = modifiers;
}
//////////////////////////////////////////////////////////////////

/**
 * Pico wrapped with singleton
 */
class PicoSingleton : public Pico {
public:

    /**
     * only method to get singleton handle to this object
     */
    static PicoSingleton & instance();

    /**
     * explicit destruction
     */
    static void destroy();

private:
    /**
     * prevent outside construction
     */
    PicoSingleton();

    /**
     * prevent compile-time deletion
     */
    virtual ~PicoSingleton();

    /**
     * prevent copy-constructor
     */
    PicoSingleton( const PicoSingleton& ) ;

    /**
     * prevent assignment operator
     */
    PicoSingleton& operator=( const PicoSingleton & ) ;

    /**
     * sole instance of this object; static assures 0 initialization for free
     */
    static PicoSingleton * single_instance;
};

PicoSingleton & PicoSingleton::instance() {
    if ( 0 == single_instance ) {
        single_instance = new PicoSingleton();
    }
    return *single_instance;
}

void PicoSingleton::destroy() {
    if ( single_instance ) {
        delete single_instance;
        single_instance = 0;
    }
}

PicoSingleton::PicoSingleton() {
}

PicoSingleton::~PicoSingleton() {
}

PicoSingleton * PicoSingleton::single_instance;
//////////////////////////////////////////////////////////////////



int main( int argc, const char ** argv )
{
    NanoSingleton::setArgs( argc, argv );

    NanoSingleton & nano = NanoSingleton::instance();

    int res;

    //
    if ( (res = nano.parse_commandline_arguments()) < 0 ) {
        nano.destroy();
        if ( res == -666 ) {
            return 0;
        }
        nano.PrintUsage();
        return 127; // command not found
    }

    //
    unsigned char * words   = 0;
    unsigned int    length  = 0;
    if ( nano.ProduceInput( &words, &length ) < 0 ) {
        nano.destroy();
        return 65; // data format error
    }

    //
    PicoSingleton & pico = PicoSingleton::instance();
    pico.setLangFilePath( nano.getLangFilePath() );
    pico.setOutFilename( nano.outFilename() );

    if ( pico.setVoice( nano.getVoice() ) < 0 ) {
        fprintf( stderr, "set voice failed, with: \"%s\n\"", nano.getVoice() );
        pico.destroy();
        nano.destroy();
        return 127; // command not found
    }

    if ( nano.writingWaveFile() ) {
        pico.writeWavePcm();
    }
    pico.setListener( nano.getListener() );
    pico.addModifiers( nano.getModifiers() );

    //
    if ( pico.initializeSystem() < 0 ) {
        fprintf( stderr, " * problem initializing Svox Pico\n" );
        pico.destroy();
        nano.destroy();
        return 126; // command found but not executable
    }

    //
    pico.sendTextForProcessing( words, length );

    //
    pico.process();

    //
    pico.cleanup();

    //
    pico.destroy();
    nano.destroy();
    return 0;
}

