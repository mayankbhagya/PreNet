#include <gtk-2.0/gtk/gtk.h>
#include <poppler/glib/poppler.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 20002
#define AUDIO_PORT 20001

#define ALSA_PCM_NEW_HW_PARAMS_AP
#define MIN_AB(a,b) ((a)<(b))?(a):(b)
#define MAX_AB(a,b) ((a)>(b))?(a):(b)

const char* hostip;

GtkBuilder *builder;

PopplerDocument *pdf_document;
int currPage=0;
gchar *file_uri;

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond;
int last;

struct packet{
char command[10];
int arg;
};

void
prenet_goto_page(PopplerDocument * pdf_document, gint index) {
    
    PopplerPage * pdf_page;
    double height=400, width=400;
    GdkPixbuf *pdf_page_image,*blank_image;
    GtkWidget *image_widget;

    image_widget = GTK_WIDGET (gtk_builder_get_object (builder, "image1"));
    
    if(pdf_document != NULL) {
        pdf_page = poppler_document_get_page(pdf_document, index);
        poppler_page_get_size (pdf_page, &width, &height);
        
        pdf_page_image = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, (int)width, (int)height);
        
        poppler_page_render_to_pixbuf (pdf_page, 0, 0, (int)width, (int)height, 1, 0, pdf_page_image);
        gtk_image_set_from_pixbuf ((GtkImage *)image_widget,pdf_page_image);
    }
    else {
        blank_image = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, (int)width, (int)height);
        gdk_pixbuf_fill(blank_image,0xefebe7ff);
        gtk_image_set_from_pixbuf ((GtkImage *)image_widget,blank_image);
    }
    
}

void 
next_page (gpointer user_data, GtkObject *object)
{
    int no_of_pages;
    GtkWidget *button;
    char buffer[6];

    no_of_pages = poppler_document_get_n_pages(pdf_document);
    currPage = MIN_AB(no_of_pages-1,currPage+1);    
    prenet_goto_page(pdf_document, currPage);
    
    button = GTK_WIDGET (gtk_builder_get_object (builder, "label4"));
    sprintf(buffer,"%d",currPage+1);
    gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
    
    pthread_cond_signal( &cond );
    
}

void 
prev_page (gpointer user_data, GtkObject *object)
{
    GtkWidget *button;
    char buffer[6];

    currPage = MAX_AB(0,currPage-1);
    prenet_goto_page (pdf_document, currPage);

    button = GTK_WIDGET (gtk_builder_get_object (builder, "label4"));
    sprintf(buffer,"%d",currPage+1);
    gtk_label_set_label ( (GtkLabel *)button, (const gchar *)buffer);
    
    pthread_cond_signal (&cond);
}

void 
filechooser_select (gpointer user_data, GtkObject *object)
{
    gtk_dialog_response (GTK_DIALOG(user_data),1);
}

void 
filechooser_cancel ( gpointer user_data,GtkObject *object)
{
    gtk_dialog_response(GTK_DIALOG(user_data),0);
}

void *
clientHandler(void *sock_id) {
    int client = *((int *)sock_id), n=1;
    char *filename = (char *)(file_uri+7);
    char data[1024] = "start";
    FILE * fp;
    
    struct packet p;
    strcpy(p.command, "file");
    send(client, &p, sizeof(p), 0);
    
    fp=fopen(filename,"rb");
    
    if(errno) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }
    while(n = fread_unlocked(data, 1, 1024, fp)) {
        send(client, data, n, 0);
        usleep(2000*100);
    }
    fclose(fp);


    while(1)
    {
        pthread_cond_wait( &cond, &mut );
        
        if(!last) {
            strcpy(p.command, "page");
            p.arg = currPage;
            send(client, &p, sizeof(p), 0);
        }
        else {
            strcpy(p.command, "close");
            send(client, &p, sizeof(p), 0);
            break;
        }
    }
    
    shutdown(client,SHUT_RDWR);
    close(client);
    pthread_exit(0);
    
}

void *
client_audio_Handler(void *cli_addr) {

  long loops;
  int sd,rc,size,dir;
  snd_pcm_t *handle;
  snd_pcm_hw_params_t *params;
  unsigned int val;
  snd_pcm_uframes_t frames;
  char *buffer;

  struct sockaddr_in client = *(struct sockaddr_in *)cli_addr,server;
  int id,id1,a,tid;

  if((sd = socket(PF_INET, SOCK_DGRAM, 0))== -1)
     perror("Socket");
  
  client.sin_port = htons(AUDIO_PORT);

	 /* Open PCM device for recording (capture). */
  rc = snd_pcm_open(&handle, "default",
                    SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    fprintf(stderr,
            "unable to open pcm device: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);

  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);

  /* Set the desired hardware parameters. */

  /* Interleaved mode */
  snd_pcm_hw_params_set_access(handle, params,
                      SND_PCM_ACCESS_RW_INTERLEAVED);

  /* Signed 16-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params,
                              SND_PCM_FORMAT_S16_LE);

  /* Two channels (stereo) */
  snd_pcm_hw_params_set_channels(handle, params, 2);

  /* 44100 bits/second sampling rate (CD quality) */
  val = 44100;
  snd_pcm_hw_params_set_rate_near(handle, params,
                                  &val, &dir);

  /* Set period size to 32 frames. */
  frames = 32;
  snd_pcm_hw_params_set_period_size_near(handle,
                               params, &frames, &dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr,
            "unable to set hw parameters: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Use a buffer large enough to hold one period */
  snd_pcm_hw_params_get_period_size(params,
                                      &frames, &dir);
  size = frames * 32; /* 2 bytes/sample, 2 channels */
  buffer = (char *) malloc(size);

  /* We want to loop for 5 seconds */
  snd_pcm_hw_params_get_period_time(params,
                                         &val, &dir);

  while(1){
    loops = 5000000 / val;
    while (loops > 0) {
      loops--;
      rc = snd_pcm_readi(handle, buffer, frames);
      if (rc == -EPIPE) {
        /* EPIPE means overrun */
        fprintf(stderr, "overrun occurred\n");
        snd_pcm_prepare(handle);
      } else if (rc < 0) {
        fprintf(stderr,
                "error from read: %s\n",
                snd_strerror(rc));
      } else if (rc != (int)frames) {
        fprintf(stderr, "short read, read %d frames\n", rc);
      }
      //g_print("Sent:%s\n",buffer);
      rc = sendto(sd, buffer, size,0,(struct sockaddr *)&client,sizeof(struct sockaddr));
      if (rc != size)
        fprintf(stderr,
                "short write: wrote %d bytes\n", rc);
    }
  }
  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  free(buffer);

  return 0;
}

void *
host_network_module (void *arg) {
        int sd, fd, nd, si, childid,reuse_addr=1;
        struct sockaddr_in my_addr;
        
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(PORT);
        my_addr.sin_addr.s_addr = INADDR_ANY;
        
        if((sd = socket(PF_INET, SOCK_STREAM, 0))== -1)
            perror("Socket");
        
        setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int));
        
        if((bind(sd,(struct sockaddr *)&my_addr,sizeof(struct sockaddr)))==-1)
            perror("Bind");
        
        if((listen(sd,10))==-1)
            perror("Listen");
        
        si=sizeof(struct sockaddr_in);
        while (1)
        {
            if((nd=accept(sd,(struct sockaddr *)&my_addr,(socklen_t *) &si))==-1)
                perror("Accept");
            pthread_create((pthread_t *)&childid,NULL,client_audio_Handler,(void *)&my_addr);
            pthread_create((pthread_t *)&childid,NULL,clientHandler,(void *)&nd);
        }
        
        shutdown(sd,SHUT_RDWR);
        close(sd);
        pthread_exit(0);
}

void 
start_host (gpointer user_data, GtkObject *object)
{
        GtkWidget *button;
        gboolean bool_is_host,bool_is_start_on;
        gchar buffer[6];
        int no_of_pages, threadid=-1;
        
        button = GTK_WIDGET (object);
        bool_is_start_on = gtk_toggle_button_get_active((GtkToggleButton *)button);
        
        button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_host"));
        bool_is_host = gtk_toggle_button_get_active((GtkToggleButton *)button);
        
        if(bool_is_host && bool_is_start_on) {
            gint result = gtk_dialog_run (GTK_DIALOG(user_data));
            switch(result) {
                case 1:
                last = 0;
                currPage = 0;
                
                gtk_widget_hide(user_data);
                button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_connect"));
                gtk_widget_set_sensitive(button,FALSE);
                
                button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_host"));
                gtk_widget_set_sensitive(button,FALSE);
                
                button = GTK_WIDGET (gtk_builder_get_object (builder, "button1"));
                gtk_widget_set_sensitive(button,TRUE);

                button = GTK_WIDGET (gtk_builder_get_object (builder, "button2"));
                gtk_widget_set_sensitive(button,TRUE);
                
                file_uri = gtk_file_chooser_get_uri((GtkFileChooser *)user_data);
                pdf_document = poppler_document_new_from_file (file_uri,NULL,NULL);
                prenet_goto_page(pdf_document,currPage);
                
                button = GTK_WIDGET (gtk_builder_get_object (builder, "label4"));
                sprintf(buffer,"%d",currPage+1);
                gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
                
                button = GTK_WIDGET (gtk_builder_get_object (builder, "label3"));
                no_of_pages = poppler_document_get_n_pages(pdf_document);
                sprintf(buffer,"%d",no_of_pages);
                gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
                
                //Start network module
                pthread_create((pthread_t *)&threadid, NULL, host_network_module, NULL);
                break;
                
                default:
                gtk_widget_hide(user_data);
                button = GTK_WIDGET (gtk_builder_get_object (builder, "start"));
                gtk_toggle_button_set_active((GtkToggleButton *)button, FALSE);
                
            }
        }
        else if (!bool_is_start_on) {
            prenet_goto_page(NULL,0);
            last = 1;
            pthread_cond_signal( &cond );
            
            button = GTK_WIDGET (gtk_builder_get_object (builder, "label3"));
            sprintf(buffer,"%d",0);
            gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);

            button = GTK_WIDGET (gtk_builder_get_object (builder, "label4"));
            sprintf(buffer,"%d",0);
            gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
            
            button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_connect"));
            gtk_widget_set_sensitive(button,TRUE);
                
            button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_host"));
            gtk_widget_set_sensitive(button,TRUE);
               
            button = GTK_WIDGET (gtk_builder_get_object (builder, "button1"));
            gtk_widget_set_sensitive(button,FALSE);

            button = GTK_WIDGET (gtk_builder_get_object (builder, "button2"));
            gtk_widget_set_sensitive(button,FALSE); 
            
            if(threadid!=-1) pthread_cancel((pthread_t)threadid);           
            
        }
}

void 
inputdialog_ok ( gpointer user_data, GtkObject *object)
{
    gtk_dialog_response(GTK_DIALOG(user_data),1);
}

void 
inputdialog_cancel ( gpointer user_data, GtkObject *object)
{
    gtk_dialog_response(GTK_DIALOG(user_data),0);
}

void *
client_network_module (void *arg) {
                    int sock, bytes_recieved;
                    char data[1024], no_of_pages;
                    struct sockaddr_in server_addr;  
                    struct packet p;
                    FILE *fp;
                    GtkWidget *button;
                    gchar buffer[6];

                    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
                        perror("Socket");
                        
                    server_addr.sin_family = AF_INET;     
                    server_addr.sin_port = htons(PORT); 
                    server_addr.sin_addr.s_addr = inet_addr(hostip);
                    
                    
                    
                    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) 
                        perror("Connect");
                    

                    bytes_recieved = recv(sock, &p, sizeof(p),0);

                    while( strcmp(p.command, "close") != 0)
                    {
                        if(strcmp(p.command,"file") == 0)
                        {
                            fp=fopen("temp.pdf","wb");
                            bytes_recieved = 1024;
                            while(bytes_recieved == 1024)
                            {   
                                bytes_recieved = recv(sock, data, 1024, 0);
                                fwrite(data, 1, bytes_recieved, fp);
                                if(errno) perror("fwrite");
                            }
                            fwrite(data, 1, bytes_recieved, fp);
                            
                            fclose(fp);
                            file_uri = malloc(100);
                            strcpy(file_uri, "file://");
                            strcat(file_uri, (char *)get_current_dir_name());
                            strcat(file_uri, "/temp.pdf");
                            
                            pdf_document = poppler_document_new_from_file (file_uri,NULL,NULL);
                            prenet_goto_page(pdf_document,currPage);     
                            
                            button = GTK_WIDGET (gtk_builder_get_object (builder, "label4"));
                            sprintf(buffer,"%d",currPage+1);
                            gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
                
                            button = GTK_WIDGET (gtk_builder_get_object (builder, "label3"));
                            no_of_pages = poppler_document_get_n_pages(pdf_document);
                            sprintf(buffer,"%d",no_of_pages);
                            gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);                       
                        }
                        
                        else if(strcmp(p.command,"page") == 0)
                        {
                            currPage = p.arg;
                            prenet_goto_page(pdf_document, currPage);
                            
                            button = GTK_WIDGET (gtk_builder_get_object (builder, "label4"));
                            sprintf(buffer,"%d",currPage+1);
                            gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
                
                            button = GTK_WIDGET (gtk_builder_get_object (builder, "label3"));
                            no_of_pages = poppler_document_get_n_pages(pdf_document);
                            sprintf(buffer,"%d",no_of_pages);
                            gtk_label_set_label((GtkLabel *)button, (const gchar *)buffer);
                        }
                        
                        bytes_recieved = recv(sock, &p, sizeof(p),0);
                    }
                    
                    prenet_goto_page(NULL, 0);
                    
                    shutdown(sock,SHUT_RDWR);
                    close(sock);
                    pthread_exit(0);
}

void *
client_audio_network_module(void *arg) {

  int sock, bytes_recieved;
  char data[1024], no_of_pages;
  struct sockaddr_in server_addr;  
  struct packet p;
  FILE *fp;
  GtkWidget *button;
  
  if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) 
      perror("Socket");
      
  server_addr.sin_family = AF_INET;     
  server_addr.sin_port = htons(AUDIO_PORT); 
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) 
      perror("Connect");
                    


  char ch;
  long loops,loop1;
  int rc,size;
  snd_pcm_t *handle;
  snd_pcm_hw_params_t *params;
  unsigned int val;
  int dir;
  snd_pcm_uframes_t frames;
  char *buffer;
  
	 /* Open PCM device for playback. */
  rc = snd_pcm_open(&handle, "default",
                    SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    fprintf(stderr,
            "unable to open pcm device: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);

  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);

  /* Set the desired hardware parameters. */

  /* Interleaved mode */
  snd_pcm_hw_params_set_access(handle, params,
                      SND_PCM_ACCESS_RW_INTERLEAVED);

  /* Signed 16-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params,
                              SND_PCM_FORMAT_S16_LE);

  /* Two channels (stereo) */
  snd_pcm_hw_params_set_channels(handle, params, 2);

  /* 44100 bits/second sampling rate (CD quality) */
  val = 44100;
  snd_pcm_hw_params_set_rate_near(handle, params,
                                  &val, &dir);

  /* Set period size to 32 frames. */
  frames = 32;
  snd_pcm_hw_params_set_period_size_near(handle,
                              params, &frames, &dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr,
            "unable to set hw parameters: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Use a buffer large enough to hold one period */
  snd_pcm_hw_params_get_period_size(params, &frames,
                                    &dir);
  size = frames * 32; /* 2 bytes/sample, 2 channels */
  buffer = (char *) malloc(size);

  /* We want to loop for 5 seconds */
  snd_pcm_hw_params_get_period_time(params,
                                    &val, &dir);
  /* 5 seconds in microseconds divided by
   * period time */
  while(1){
  loops = 5000000 / val;
    while (loops > 0) {
      loops--;
      rc = recvfrom(sock, buffer, size,0, NULL, NULL);
      //g_print("Recd:%s\n",buffer);
      if (rc == 0) {
        fprintf(stderr, "end of file on input\n");
        break;
      } else if (rc != size) {
        fprintf(stderr,
                "short read: read %d bytes\n", rc);
      }
      rc = snd_pcm_writei(handle, buffer, frames);
      if (rc == -EPIPE) {
        /* EPIPE means underrun */
        fprintf(stderr, "underrun occurred\n");
        snd_pcm_prepare(handle);
      } else if (rc < 0) {
        fprintf(stderr,
                "error from writei: %s\n",
                snd_strerror(rc));
      }  else if (rc != (int)frames) {
        fprintf(stderr,
                "short write, write %d frames\n", rc);
      }
    }
  }
  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  free(buffer);

  return 0;
}

void 
start_client ( gpointer user_data, GtkObject *object)
{
        GtkWidget *button;
        gboolean bool_is_client, bool_is_start_on; 
        gchar buffer[6];
        int no_of_pages, client_threadid;

        button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_connect"));
        bool_is_client = gtk_toggle_button_get_active((GtkToggleButton *)button);

        button = GTK_WIDGET (object);
        bool_is_start_on = gtk_toggle_button_get_active((GtkToggleButton *)button);
        
        if(bool_is_client && bool_is_start_on) {
            gint result = gtk_dialog_run (GTK_DIALOG(user_data));
            gtk_widget_hide((GtkWidget *)user_data);
            
            switch (result) {
                case 1:
                    button = GTK_WIDGET (gtk_builder_get_object (builder, "entry2"));
                    hostip = (char *)gtk_entry_get_text((GtkEntry *)button);
                    
                    button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_host"));
                    gtk_widget_set_sensitive(button,FALSE);
                    
                    button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_connect"));
                    gtk_widget_set_sensitive(button,FALSE);        
                    
                    //Start network module
                    pthread_create((pthread_t *)&client_threadid,NULL,client_network_module,NULL);
                    pthread_create((pthread_t *)&client_threadid,NULL,client_audio_network_module,NULL);
                    break;
                
                default:
                    button = GTK_WIDGET (gtk_builder_get_object (builder, "start"));
                    gtk_toggle_button_set_active((GtkToggleButton *)button, FALSE);                
                    
            }
        }
        else if(!bool_is_start_on) {
            prenet_goto_page(NULL,0);
        }
}

gboolean
key_press (gpointer user_data, GdkEventKey *key, GtkObject *object)
{
    GtkWidget *button;
    gboolean bool_is_host;
    
    button = GTK_WIDGET (gtk_builder_get_object (builder, "radio_host"));
    bool_is_host = gtk_toggle_button_get_active((GtkToggleButton *)button);


    if(bool_is_host) {
        if(key->keyval == 65363||key->keyval == 65364) {
            next_page(NULL,NULL);
        }
        else if(key->keyval == 65362||key->keyval == 65361) {
            prev_page(NULL,NULL);
        }
    }
}

void
prenet_init()
{
        GtkWidget *widget;
        
        //GTK initialisation
        gtk_init (NULL, NULL);
        
        //Get builder from xml file
        builder = gtk_builder_new ();
        gtk_builder_add_from_file (builder, "prenet.xml", NULL);

        //Disable all widgets that are not required
        widget = GTK_WIDGET (gtk_builder_get_object (builder, "button1"));
        gtk_widget_set_sensitive (widget,FALSE);

        widget = GTK_WIDGET (gtk_builder_get_object (builder, "button2"));
        gtk_widget_set_sensitive (widget,FALSE);
        
        //Get window widget
        widget = GTK_WIDGET (gtk_builder_get_object (builder, "window1"));
        
        //Connect all signals
        gtk_builder_connect_signals (builder, NULL);

        //show window
        gtk_widget_show (widget);
}

int
main (int argc, char *argv[])
{
        setbuf(stdout,NULL);
        prenet_init();
        
        prenet_goto_page(NULL,0);
        
        gtk_main ();
        return 0;
}

