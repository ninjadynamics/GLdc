#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glu.h>
#include <GL/glkos.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <kos/mutex.h>
#include <kos/thread.h>

#include "libdcmc/snddrv.h"
#include "dreamroqlib.h"
#include "libdcmc/dc_timer.h"

#ifdef __DREAMCAST__
extern uint8 romdisk[];
KOS_INIT_ROMDISK(romdisk);
#define VIDEO_FILENAME "/rd/video.roq"
#else
#define VIDEO_FILENAME "../samples/dreamroq/romdisk/video.roq"
#endif

maple_device_t  *cont;
cont_state_t    *state;

/* Audio Global variables */
#define PCM_BUF_SIZE                1024*1024
static unsigned char *pcm_buf       = NULL;
static int pcm_size                 = 0;
static int audio_init               = 0;
static mutex_t * pcm_mut;

/* Video Global variables */
static int current_frame            = 0;
static int graphics_initialized     = 0;
static float video_delay            = 0.0f;
static int frame                    = 0;
static const float VIDEO_RATE       = 30.0f; /* Video FPS */

static GLint frameTexture[2];
//static GLVertexKOS vertices[4];

// maybe should aling this better?
typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float u, v;
} UV;

static Vec3    vertices[4];
static UV 	    uv[4];

static void snd_thd()
{
    do
    {
        /* Wait for AICA Driver to request some samples */   
        while( snddrv.buf_status != SNDDRV_STATUS_NEEDBUF ) 
            thd_pass();    

        /* Wait for RoQ Decoder to produce enough samples */
        while( pcm_size < snddrv.pcm_needed )
        {
            if( snddrv.dec_status == SNDDEC_STATUS_DONE )
                goto done;   
            thd_pass();    
        }

        /* Copy the Requested PCM Samples to the AICA Driver */          
        mutex_lock( pcm_mut );
        memcpy( snddrv.pcm_buffer, pcm_buf, snddrv.pcm_needed );
        
        /* Shift the Remaining PCM Samples Back */
        pcm_size -= snddrv.pcm_needed;
        memmove( pcm_buf, pcm_buf+snddrv.pcm_needed, pcm_size );
        mutex_unlock( pcm_mut );
         
        /* Let the AICA Driver know the PCM samples are ready */ 
        snddrv.buf_status = SNDDRV_STATUS_HAVEBUF;
                    
    } while( snddrv.dec_status == SNDDEC_STATUS_STREAMING );     
    done:
    snddrv.dec_status = SNDDEC_STATUS_NULL;
}

static int  renderGLdc_cb(unsigned short *buf, int width, int height, int stride, int texture_height)
{
    
    /* send the video frame as a texture over to video RAM */
    //pvr_txr_load(buf, textures[current_frame], stride * texture_height * 2);
    glBindTexture(GL_TEXTURE_2D, frameTexture[current_frame]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 512, 512, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, buf);
              
    /* Delay the frame to match Frame Rate */
    frame_delay(VIDEO_RATE, video_delay, ++frame);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer		(3, GL_FLOAT, 0, vertices);
	glTexCoordPointer	(2, GL_FLOAT, 0, uv);
	
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);

    
    glKosSwapBuffers();

    /*
    if (current_frame)
        current_frame = 0;
    else
        current_frame = 1;
    */

    return ROQ_SUCCESS;
}

static int  audio_cb( unsigned char *buf, int size, int channels)
{


    /* Copy the decoded PCM samples to our local PCM buffer */
    mutex_lock( pcm_mut );          
    memcpy(  pcm_buf+pcm_size, buf, size);
    pcm_size += size;
    mutex_unlock( pcm_mut );
       
    return ROQ_SUCCESS;    
}

static int  quit_cb()
{
    /*
    state   = (cont_state_t *)maple_dev_status(cont);
    
    if(state->buttons & CONT_START)
        return 1;
    else
        return 0;
    */  
}

int         main(int argc, char **argv)
{
    printf("--- DreamRoQ Player for Dreamcast\n");
    glKosInit();
    //cont    = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    //state   = (cont_state_t *)maple_dev_status(cont);

    if(!graphics_initialized) {
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);		// This Will Clear The Background Color To Black
        glClearDepth(1.0);				// Enables Clearing Of The Depth Buffer
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	    //glDisable(GL_DEPTH_TEST);
        //glEnable(GL_NORMALIZE);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity(); // Reset The Projection Matrix
        glOrtho(0.0, 640.0, 0.0, 480.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glDisable(GL_LIGHTING);
        glEnable(GL_TEXTURE_2D);
        glGenTextures(2, frameTexture);
        glBindTexture(GL_TEXTURE_2D, frameTexture[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 512, 512, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        
        glBindTexture(GL_TEXTURE_2D, frameTexture[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 512, 512, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);

        video_delay = (float)dc_get_time();

        float   w = 512;
        float   h = 512;
        int     v = 0;

        vertices[v].x = 0;
        vertices[v].y = 0;
        vertices[v].z = 0;
        uv[v].u = 0.0f;
        uv[v].v = 1.0f;
        v++;

        vertices[v].x = 0;
        vertices[v].y = 480;
        vertices[v].z = 0;
        uv[v].u = 0.0f;
        uv[v].v = 0.0f;
        v++;

        vertices[v].x = 640;
        vertices[v].y = 0;
        vertices[v].z = 0;
        uv[v].u = 1.0f;
        uv[v].v = 1.0f;
        v++;

        vertices[v].x = 640;
        vertices[v].y = 480;
        vertices[v].z = 0;
        uv[v].u = 1.0f;
        uv[v].v = 0.0f;
        v++;

        GLfloat drawColor[4]    = {1.0f, 1.0f, 1.0f, 1.0f};
        GLfloat emissionColor[4] = {0.0, 0.0, 0.0, 1.0f};
	    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, drawColor);
	    glMaterialfv(GL_FRONT, GL_SPECULAR, drawColor);
	    glMaterialfv(GL_FRONT, GL_EMISSION, emissionColor);

        graphics_initialized = 1;
    }

    /*
    if(!audio_init) {
        // allocate PCM buffer
        pcm_buf = malloc(PCM_BUF_SIZE);
        if( pcm_buf == NULL )
            return ROQ_NO_MEMORY;
        
        // Start AICA Driver 
        // Audio rate, channel number
        snddrv_start( 22050, 2); 
        snddrv.dec_status = SNDDEC_STATUS_STREAMING; 
        
        // Create a thread to stream the samples to the AICA
        thd_create(0, snd_thd, NULL);
        
        // Create a mutex to handle the double-threaded buffer
        //pcm_mut = mutex_create();
        
        audio_init = 1;
    }
    */

    printf("--- Playing video using DreamRoQ\n");
    int status = dreamroq_play(VIDEO_FILENAME, 0, renderGLdc_cb, 0, 0);
    printf("dreamroq_play() status = %d\n", status);  

    /*
    if(audio_init) {
      snddrv.dec_status = SNDDEC_STATUS_DONE;  // Singal audio thread to stop
      while( snddrv.dec_status != SNDDEC_STATUS_NULL )
         thd_pass();     
      free( pcm_buf );
      pcm_buf = NULL;
      pcm_size = 0;  
      mutex_destroy(pcm_mut);                  // Destroy the PCM mutex 
      snddrv_exit();                           // Exit the AICA Driver   
    } 

    if(graphics_initialized) {
        glDeleteTextures(2, frameTexture);
        glEnable(GL_LIGHTING);
    } 
    */
    return status;
}

