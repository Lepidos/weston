/*
 * Copyright © 2010 Kristian Høgsberg
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "compositor.h"

struct wlsc_shm_buffer {
	struct wl_buffer buffer;
	int32_t stride;
	void *data;
	int mapped;
};

static void
destroy_buffer(struct wl_resource *resource, struct wl_client *client)
{
	struct wlsc_shm_buffer *buffer =
		container_of(resource, struct wlsc_shm_buffer, buffer.resource);
	struct wlsc_compositor *compositor = 
		(struct wlsc_compositor *) buffer->buffer.compositor;
	struct wlsc_surface *es;

	if (buffer->mapped)
		munmap(buffer->data, buffer->stride * buffer->buffer.height);
	else
		free(buffer->data);

	wl_list_for_each(es, &compositor->surface_list, link)
		if (es->buffer == (struct wl_buffer *) buffer)
			es->buffer = NULL;

	free(buffer);
}

static void
buffer_damage(struct wl_client *client, struct wl_buffer *buffer_base,
	      int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wlsc_shm_buffer *buffer =
		(struct wlsc_shm_buffer *) buffer_base;
	struct wlsc_compositor *compositor = 
		(struct wlsc_compositor *) buffer->buffer.compositor;
	struct wlsc_surface *es;

	wl_list_for_each(es, &compositor->surface_list, link) {
		if (es->buffer != buffer_base)
			continue;

		glBindTexture(GL_TEXTURE_2D, es->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     buffer->buffer.width, buffer->buffer.height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, buffer->data);
		/* Hmm, should use glTexSubImage2D() here but GLES2 doesn't
		 * support any unpack attributes except GL_UNPACK_ALIGNMENT. */
	}
}

static void
buffer_destroy(struct wl_client *client, struct wl_buffer *buffer)
{
	wl_resource_destroy(&buffer->resource, client);
}

const static struct wl_buffer_interface buffer_interface = {
	buffer_damage,
	buffer_destroy
};

int
wlsc_is_shm_buffer(struct wl_buffer *buffer)
{
	return buffer->resource.object.implementation == 
		(void (**)(void)) &buffer_interface;
}

void
wlsc_shm_buffer_attach(struct wl_buffer *buffer_base,
		       struct wl_surface *surface)
{
	struct wlsc_surface *es = (struct wlsc_surface *) surface;
	struct wlsc_shm_buffer *buffer =
		(struct wlsc_shm_buffer *) buffer_base;

	glBindTexture(GL_TEXTURE_2D, es->texture);

	/* Unbind any EGLImage texture that may be bound, so we don't
	 * overwrite it.*/
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
		     0, 0, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
		     buffer->buffer.width, buffer->buffer.height, 0,
		     GL_BGRA_EXT, GL_UNSIGNED_BYTE, buffer->data);
	es->visual = buffer->buffer.visual;
}

static struct wlsc_shm_buffer *
wlsc_shm_buffer_init(struct wlsc_compositor *compositor,
		     int32_t width, int32_t height,
		     int32_t stride, struct wl_visual *visual,
		     void *data)
{
	struct wlsc_shm_buffer *buffer;

	buffer = malloc(sizeof *buffer);
	if (buffer == NULL)
		return NULL;

	buffer->buffer.compositor = &compositor->compositor;
	buffer->buffer.width = width;
	buffer->buffer.height = height;
	buffer->buffer.visual = visual;
	buffer->stride = stride;
	buffer->data = data;

	buffer->buffer.resource.object.interface = &wl_buffer_interface;
	buffer->buffer.resource.object.implementation = (void (**)(void))
		&buffer_interface;

	buffer->buffer.resource.destroy = destroy_buffer;

	return buffer;
}

static void
shm_create_buffer(struct wl_client *client, struct wl_shm *shm,
		  uint32_t id, int fd, int32_t width, int32_t height,
		  uint32_t stride, struct wl_visual *visual)
{
	struct wlsc_compositor *compositor =
		container_of((struct wlsc_shm *) shm,
			     struct wlsc_compositor, shm);
	struct wlsc_shm_buffer *buffer;
	void *data;

	/* FIXME: Define a real exception event instead of abusing the
	 * display.invalid_object error */
	if (visual->object.interface != &wl_visual_interface) {
		wl_client_post_event(client,
				     (struct wl_object *) compositor->wl_display,
				     WL_DISPLAY_INVALID_OBJECT, 0);
		fprintf(stderr, "invalid visual in create_buffer\n");
		close(fd);
		return;
	}

	if (width < 0 || height < 0 || stride < width) {
		wl_client_post_event(client,
				     (struct wl_object *) compositor->wl_display,
				     WL_DISPLAY_INVALID_OBJECT, 0);
		fprintf(stderr,
			"invalid width, height or stride in create_buffer\n");
		close(fd);
		return;
	}

	data = mmap(NULL, stride * height, PROT_READ, MAP_SHARED, fd, 0);

	close(fd);
	if (data == MAP_FAILED) {
		/* FIXME: Define a real exception event instead of
		 * abusing this one */
		wl_client_post_event(client,
				     (struct wl_object *) compositor->wl_display,
				     WL_DISPLAY_INVALID_OBJECT, 0);
		fprintf(stderr, "failed to create image for fd %d\n", fd);
		return;
	}

	buffer = wlsc_shm_buffer_init(compositor, width, height,
				      stride, visual, data);
	if (buffer == NULL) {
		munmap(data, stride * height);
		wl_client_post_no_memory(client);
		return;
	}
	buffer->mapped = 1;

	buffer->buffer.resource.object.id = id;

	wl_client_add_resource(client, &buffer->buffer.resource);
}

const static struct wl_shm_interface shm_interface = {
	shm_create_buffer
};

int
wlsc_shm_init(struct wlsc_compositor *ec)
{
	struct wlsc_shm *shm = &ec->shm;

	shm->object.interface = &wl_shm_interface;
	shm->object.implementation = (void (**)(void)) &shm_interface;
	wl_display_add_object(ec->wl_display, &shm->object);
	wl_display_add_global(ec->wl_display, &shm->object, NULL);

	return 0;
}
