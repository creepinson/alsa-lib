/*
 *  Mixer Interface - main file
 *  Copyright (c) 1998/1999/2000 by Jaroslav Kysela <perex@suse.cz>
 *  Copyright (c) 2001 by Abramo Bagnara <abramo@alsa-project.org>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "mixer_local.h"

typedef struct _snd_mixer_slave {
	snd_hctl_t *hctl;
	struct list_head list;
} snd_mixer_slave_t;


typedef struct _snd_mixer_elem_bag {

} snd_mixer_elem_bag_t;


static int snd_mixer_compare_default(const snd_mixer_elem_t *c1,
				     const snd_mixer_elem_t *c2);


int snd_mixer_open(snd_mixer_t **mixerp)
{
	snd_mixer_t *mixer;
	assert(mixerp);
	mixer = calloc(1, sizeof(*mixer));
	if (mixer == NULL)
		return -ENOMEM;
	INIT_LIST_HEAD(&mixer->slaves);
	INIT_LIST_HEAD(&mixer->classes);
	INIT_LIST_HEAD(&mixer->elems);
	mixer->compare = snd_mixer_compare_default;
	*mixerp = mixer;
	return 0;
}

int snd_mixer_elem_attach(snd_mixer_elem_t *melem,
			  snd_hctl_elem_t *helem)
{
	bag_t *bag = snd_hctl_elem_get_callback_private(helem);
	int err;
	err = bag_add(bag, melem);
	if (err < 0)
		return err;
	return bag_add(&melem->helems, helem);
}

int snd_mixer_elem_detach(snd_mixer_elem_t *melem,
			  snd_hctl_elem_t *helem)
{
	bag_t *bag = snd_hctl_elem_get_callback_private(helem);
	int err;
	err = bag_del(bag, melem);
	assert(err >= 0);
	err = bag_del(&melem->helems, helem);
	assert(err >= 0);
	return 0;
}

int snd_mixer_elem_empty(snd_mixer_elem_t *melem)
{
	return bag_empty(&melem->helems);
}

static int hctl_elem_event_handler(snd_hctl_elem_t *helem,
				   snd_ctl_event_type_t event)
{
	bag_t *bag = snd_hctl_elem_get_callback_private(helem);
	int res = 0;
	switch (event) {
	case SND_CTL_EVENT_VALUE:
	case SND_CTL_EVENT_INFO:
	{
		int err = 0;
		bag_iterator_t i, n;
		bag_for_each(i, n, bag) {
			snd_mixer_elem_t *melem = bag_iterator_entry(i);
			snd_mixer_class_t *class = melem->class;
			err = class->event(class, event, helem, melem);
			if (err < 0)
				break;
		}
		break;
	}
	case SND_CTL_EVENT_REMOVE:
	{
		int err;
		bag_iterator_t i, n;
		bag_for_each(i, n, bag) {
			snd_mixer_elem_t *melem = bag_iterator_entry(i);
			snd_mixer_class_t *class = melem->class;
			err = class->event(class, event, helem, melem);
			if (err < 0)
				res = err;
		}
		assert(bag_empty(bag));
		bag_free(bag);
		break;

	}
	default:
		assert(0);
		break;
	}
	return res;
}

static int hctl_event_handler(snd_hctl_t *hctl, snd_ctl_event_type_t event,
			      snd_hctl_elem_t *elem)
{
	snd_mixer_t *mixer = snd_hctl_get_callback_private(hctl);
	int res = 0;
	switch (event) {
	case SND_CTL_EVENT_ADD:
	{
		struct list_head *pos, *next;
		bag_t *bag;
		int err = bag_new(&bag);
		if (err < 0)
			return err;
		snd_hctl_elem_set_callback(elem, hctl_elem_event_handler);
		snd_hctl_elem_set_callback_private(elem, bag);
		list_for_each(pos, next, &mixer->classes) {
			snd_mixer_class_t *c;
			c = list_entry(pos, snd_mixer_class_t, list);
			err = c->event(c, event, elem, NULL);
			if (err < 0)
				res = err;
		}
		break;
	}
	default:
		assert(0);
		break;
	}
	return res;
}


int snd_mixer_attach(snd_mixer_t *mixer, const char *name)
{
	snd_mixer_slave_t *slave;
	snd_hctl_t *hctl;
	int err;
	slave = calloc(1, sizeof(*slave));
	if (slave == NULL)
		return -ENOMEM;
	err = snd_hctl_open(&hctl, name);
	if (err < 0) {
		free(slave);
		return err;
	}
	err = snd_hctl_nonblock(hctl, 1);
	if (err < 0) {
		snd_hctl_close(hctl);
		free(slave);
		return err;
	}
	snd_hctl_set_callback(hctl, hctl_event_handler);
	snd_hctl_set_callback_private(hctl, mixer);
	slave->hctl = hctl;
	list_add_tail(&slave->list, &mixer->slaves);
	return 0;
}

int snd_mixer_detach(snd_mixer_t *mixer, const char *name)
{
	struct list_head *pos, *next;
	list_for_each(pos, next, &mixer->slaves) {
		snd_mixer_slave_t *s;
		s = list_entry(pos, snd_mixer_slave_t, list);
		if (strcmp(name, snd_hctl_name(s->hctl)) == 0) {
			snd_hctl_close(s->hctl);
			list_del(pos);
			free(s);
			return 0;
		}
	}
	return -ENOENT;
}

int snd_mixer_throw_event(snd_mixer_t *mixer, snd_ctl_event_type_t event,
			  snd_mixer_elem_t *elem)
{
	mixer->events++;
	if (mixer->callback)
		return mixer->callback(mixer, event, elem);
	return 0;
}

int snd_mixer_elem_throw_event(snd_mixer_elem_t *elem,
			       snd_ctl_event_type_t event)
{
	elem->class->mixer->events++;
	if (elem->callback)
		return elem->callback(elem, event);
	return 0;
}

static int _snd_mixer_find_elem(snd_mixer_t *mixer, snd_mixer_elem_t *elem, int *dir)
{
	unsigned int l, u;
	int c = 0;
	int idx = -1;
	assert(mixer && elem);
	assert(mixer->compare);
	l = 0;
	u = mixer->count;
	while (l < u) {
		idx = (l + u) / 2;
		c = mixer->compare(elem, mixer->pelems[idx]);
		if (c < 0)
			u = idx;
		else if (c > 0)
			l = idx + 1;
		else
			break;
	}
	*dir = c;
	return idx;
}

int snd_mixer_elem_add(snd_mixer_elem_t *elem, snd_mixer_class_t *class)
{
	int dir, idx;
	snd_mixer_t *mixer = class->mixer;
	elem->class = class;

	if (mixer->count == mixer->alloc) {
		snd_mixer_elem_t **m;
		mixer->alloc += 32;
		m = realloc(mixer->pelems, sizeof(*m) * mixer->alloc);
		if (!m) {
			mixer->alloc -= 32;
			return -ENOMEM;
		}
		mixer->pelems = m;
	}
	if (mixer->count == 0) {
		list_add_tail(&elem->list, &mixer->elems);
		mixer->pelems[0] = elem;
	} else {
		idx = _snd_mixer_find_elem(mixer, elem, &dir);
		assert(dir != 0);
		if (dir > 0) {
			list_add(&elem->list, &mixer->pelems[idx]->list);
		} else {
			list_add_tail(&elem->list, &mixer->pelems[idx]->list);
			idx++;
		}
		memmove(mixer->pelems + idx + 1,
			mixer->pelems + idx,
			mixer->count - idx);
	}
	mixer->count++;
	return snd_mixer_throw_event(mixer, SND_CTL_EVENT_ADD, elem);
}

int snd_mixer_elem_remove(snd_mixer_elem_t *elem)
{
	snd_mixer_t *mixer = elem->class->mixer;
	int err, idx, dir;
	unsigned int m;
	assert(elem);
	idx = _snd_mixer_find_elem(mixer, elem, &dir);
	if (dir != 0)
		return -EINVAL;
	err = snd_mixer_elem_throw_event(elem, SND_CTL_EVENT_REMOVE);
	list_del(&elem->list);
	free(elem);
	mixer->count--;
	m = mixer->count - idx;
	if (m > 0)
		memmove(mixer->pelems + idx, mixer->pelems + idx + 1, m);
	return err;
}

int snd_mixer_elem_change(snd_mixer_elem_t *elem)
{
	return snd_mixer_elem_throw_event(elem, SND_CTL_EVENT_INFO);
}


int snd_mixer_class_register(snd_mixer_class_t *class, snd_mixer_t *mixer)
{
	struct list_head *pos, *next;
	class->mixer = mixer;
	list_add_tail(&class->list, &mixer->classes);
	if (!class->event)
		return 0;
	list_for_each(pos, next, &mixer->slaves) {
		int err;
		snd_mixer_slave_t *slave;
		snd_hctl_elem_t *elem;
		slave = list_entry(pos, snd_mixer_slave_t, list);
		elem = snd_hctl_first_elem(slave->hctl);
		while (elem) {
			err = class->event(class, SND_CTL_EVENT_ADD, elem, NULL);
			if (err < 0)
				return err;
			elem = snd_hctl_elem_next(elem);
		}
	}
	return 0;
}

int snd_mixer_class_unregister(snd_mixer_class_t *class)
{
	struct list_head *pos, *next;
	snd_mixer_t *mixer = class->mixer;
	list_for_each(pos, next, &mixer->elems) {
		snd_mixer_elem_t *e;
		e = list_entry(pos, snd_mixer_elem_t, list);
		if (e->class == class && e->private_free)
			e->private_free(e);
		snd_mixer_elem_remove(e);
	}
	if (class->private_free)
		class->private_free(class);
	list_del(&class->list);
	free(class);
	return 0;
}

int snd_mixer_load(snd_mixer_t *mixer)
{
	struct list_head *pos, *next;
	list_for_each(pos, next, &mixer->slaves) {
		int err;
		snd_mixer_slave_t *s;
		s = list_entry(pos, snd_mixer_slave_t, list);
		err = snd_hctl_load(s->hctl);
		if (err < 0)
			return err;
	}
	return 0;
}

void snd_mixer_free(snd_mixer_t *mixer)
{
	struct list_head *pos, *next;
	list_for_each(pos, next, &mixer->slaves) {
		snd_mixer_slave_t *s;
		s = list_entry(pos, snd_mixer_slave_t, list);
		snd_hctl_free(s->hctl);
	}
}

int snd_mixer_close(snd_mixer_t *mixer)
{
	int res = 0;
	assert(mixer);
	while (!list_empty(&mixer->classes)) {
		snd_mixer_class_t *c;
		c = list_entry(mixer->classes.next, snd_mixer_class_t, list);
		snd_mixer_class_unregister(c);
	}
	assert(list_empty(&mixer->elems));
	if (mixer->pelems) {
		free(mixer->pelems);
		mixer->pelems = NULL;
	}
	while (!list_empty(&mixer->slaves)) {
		int err;
		snd_mixer_slave_t *s;
		s = list_entry(mixer->slaves.next, snd_mixer_slave_t, list);
		err = snd_hctl_close(s->hctl);
		if (err < 0)
			res = err;
		list_del(&s->list);
		free(s);
	}
	free(mixer);
	return res;
}

static int snd_mixer_compare_default(const snd_mixer_elem_t *c1,
				     const snd_mixer_elem_t *c2)
{
	int d = c1->compare_weight - c2->compare_weight;
	if (d)
		return d;
	assert(c1->class && c1->class->compare);
	assert(c2->class && c2->class->compare);
	assert(c1->class == c2->class);
	return c1->class->compare(c1, c2);
}

static int snd_mixer_sort(snd_mixer_t *mixer)
{
	unsigned int k;
	int compar(const void *a, const void *b) {
		return mixer->compare(*(const snd_mixer_elem_t **) a,
				      *(const snd_mixer_elem_t **) b);
	}
	assert(mixer);
	assert(mixer->compare);
	INIT_LIST_HEAD(&mixer->elems);
	qsort(mixer->pelems, mixer->count, sizeof(snd_mixer_elem_t), compar);
	for (k = 0; k < mixer->count; k++)
		list_add_tail(&mixer->pelems[k]->list, &mixer->elems);
	return 0;
}

int snd_mixer_set_compare(snd_mixer_t *mixer, snd_mixer_compare_t msort)
{
	snd_mixer_compare_t msort_old;
	int err;

	assert(mixer);
	msort_old = mixer->compare;
	mixer->compare = msort == NULL ? snd_mixer_compare_default : msort;
	if ((err = snd_mixer_sort(mixer)) < 0) {
		mixer->compare = msort_old;
		return err;
	}
	return 0;
}

int snd_mixer_poll_descriptor(snd_mixer_t *mixer, const char *name)
{
	struct list_head *pos, *next;
	assert(mixer && name);
	list_for_each(pos, next, &mixer->slaves) {
		snd_mixer_slave_t *s;
		const char *n;
		s = list_entry(pos, snd_mixer_slave_t, list);
		n = snd_hctl_name(s->hctl);
		if (n && strcmp(name, n) == 0)
			return snd_hctl_poll_descriptor(s->hctl);
	}
	return -ENOENT;
}

snd_mixer_elem_t *snd_mixer_first_elem(snd_mixer_t *mixer)
{
	assert(mixer);
	if (list_empty(&mixer->elems))
		return NULL;
	return list_entry(mixer->elems.next, snd_mixer_elem_t, list);
}

snd_mixer_elem_t *snd_mixer_last_elem(snd_mixer_t *mixer)
{
	assert(mixer);
	if (list_empty(&mixer->elems))
		return NULL;
	return list_entry(mixer->elems.prev, snd_mixer_elem_t, list);
}

snd_mixer_elem_t *snd_mixer_elem_next(snd_mixer_elem_t *elem)
{
	assert(elem);
	if (elem->list.next == &elem->class->mixer->elems)
		return NULL;
	return list_entry(elem->list.next, snd_mixer_elem_t, list);
}

snd_mixer_elem_t *snd_mixer_elem_prev(snd_mixer_elem_t *elem)
{
	assert(elem);
	if (elem->list.prev == &elem->class->mixer->elems)
		return NULL;
	return list_entry(elem->list.prev, snd_mixer_elem_t, list);
}

int snd_mixer_handle_events(snd_mixer_t *mixer)
{
	struct list_head *pos, *next;
	assert(mixer);
	mixer->events = 0;
	list_for_each(pos, next, &mixer->slaves) {
		int err;
		snd_mixer_slave_t *s;
		s = list_entry(pos, snd_mixer_slave_t, list);
		err = snd_hctl_handle_events(s->hctl);
		if (err < 0)
			return err;
	}
	return mixer->events;
}

