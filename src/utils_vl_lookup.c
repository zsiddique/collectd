/**
 * collectd - src/utils_vl_lookup.c
 * Copyright (C) 2012  Florian Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian Forster <octo at collectd.org>
 **/

#include "collectd.h"
#include "common.h"
#include "utils_vl_lookup.h"
#include "utils_avltree.h"

#if BUILD_TEST
# define sstrncpy strncpy
# define plugin_log(s, ...) do { \
  printf ("[severity %i] ", s); \
  printf (__VA_ARGS__); \
  printf ("\n"); \
} while (0)
#endif

/*
 * Types
 */
struct lookup_s
{
  c_avl_tree_t *by_type_tree;

  lookup_class_callback_t cb_user_class;
  lookup_obj_callback_t cb_user_obj;
  lookup_free_class_callback_t cb_free_class;
  lookup_free_obj_callback_t cb_free_obj;
};

struct user_obj_s;
typedef struct user_obj_s user_obj_t;
struct user_obj_s
{
  void *user_obj;
  identifier_t ident;

  user_obj_t *next;
};

struct user_class_s
{
  void *user_class;
  identifier_t ident;
  user_obj_t *user_obj_list; /* list of user_obj */
};
typedef struct user_class_s user_class_t;

struct user_class_list_s;
typedef struct user_class_list_s user_class_list_t;
struct user_class_list_s
{
  user_class_t entry;
  user_class_list_t *next;
};

struct by_type_entry_s
{
  c_avl_tree_t *by_plugin_tree; /* plugin -> user_class_list_t */
  user_class_list_t *wildcard_plugin_list;
};
typedef struct by_type_entry_s by_type_entry_t;

/*
 * Private functions
 */
static void *lu_create_user_obj (lookup_t *obj, /* {{{ */
    data_set_t const *ds, value_list_t const *vl,
    user_class_t *user_class)
{
  user_obj_t *user_obj;

  user_obj = malloc (sizeof (*user_obj));
  if (user_obj == NULL)
  {
    ERROR ("utils_vl_lookup: malloc failed.");
    return (NULL);
  }
  memset (user_obj, 0, sizeof (*user_obj));
  user_obj->next = NULL;

  user_obj->user_obj = obj->cb_user_class (ds, vl, user_class->user_class);
  if (user_obj->user_obj == NULL)
  {
    sfree (user_obj);
    WARNING("utils_vl_lookup: User-provided constructor failed.");
    return (NULL);
  }

  sstrncpy (user_obj->ident.host,
    LU_IS_ALL (user_class->ident.host) ?  "/all/" : vl->host,
    sizeof (user_obj->ident.host));
  sstrncpy (user_obj->ident.plugin,
    LU_IS_ALL (user_class->ident.plugin) ?  "/all/" : vl->plugin,
    sizeof (user_obj->ident.plugin));
  sstrncpy (user_obj->ident.plugin_instance,
    LU_IS_ALL (user_class->ident.plugin_instance) ?  "/all/" : vl->plugin_instance,
    sizeof (user_obj->ident.plugin_instance));
  sstrncpy (user_obj->ident.type,
    LU_IS_ALL (user_class->ident.type) ?  "/all/" : vl->type,
    sizeof (user_obj->ident.type));
  sstrncpy (user_obj->ident.type_instance,
    LU_IS_ALL (user_class->ident.type_instance) ?  "/all/" : vl->type_instance,
    sizeof (user_obj->ident.type_instance));

  if (user_class->user_obj_list == NULL)
  {
    user_class->user_obj_list = user_obj;
  }
  else
  {
    user_obj_t *last = user_class->user_obj_list;
    while (last->next != NULL)
      last = last->next;
    last->next = user_obj;
  }

  return (user_obj);
} /* }}} void *lu_create_user_obj */

static user_obj_t *lu_find_user_obj (user_class_t *user_class, /* {{{ */
    value_list_t const *vl)
{
  user_obj_t *ptr;

  for (ptr = user_class->user_obj_list;
      ptr != NULL;
      ptr = ptr->next)
  {
    if (!LU_IS_ALL (ptr->ident.host)
        && (strcmp (ptr->ident.host, vl->host) != 0))
      continue;
    if (!LU_IS_ALL (ptr->ident.plugin_instance)
        && (strcmp (ptr->ident.plugin_instance, vl->plugin_instance) != 0))
      continue;
    if (!LU_IS_ALL (ptr->ident.type_instance)
        && (strcmp (ptr->ident.type_instance, vl->type_instance) != 0))
      continue;

    return (ptr);
  }

  return (NULL);
} /* }}} user_obj_t *lu_find_user_obj */

static int lu_handle_user_class (lookup_t *obj, /* {{{ */
    data_set_t const *ds, value_list_t const *vl,
    user_class_t *user_class)
{
  user_obj_t *user_obj;
  int status;

  assert (strcmp (vl->type, user_class->ident.type) == 0);
  assert (LU_IS_WILDCARD (user_class->ident.plugin)
      || (strcmp (vl->plugin, user_class->ident.plugin) == 0));

  /* When we get here, type and plugin already match the user class. Now check
   * the rest of the fields. */
  if (!LU_IS_WILDCARD (user_class->ident.type_instance)
      && (strcmp (vl->type_instance, user_class->ident.type_instance) != 0))
    return (1);
  if (!LU_IS_WILDCARD (user_class->ident.plugin_instance)
      && (strcmp (vl->plugin_instance,
          user_class->ident.plugin_instance) != 0))
    return (1);
  if (!LU_IS_WILDCARD (user_class->ident.host)
      && (strcmp (vl->host, user_class->ident.host) != 0))
    return (1);

  user_obj = lu_find_user_obj (user_class, vl);
  if (user_obj == NULL)
  {
    /* call lookup_class_callback_t() and insert into the list of user objects. */
    user_obj = lu_create_user_obj (obj, ds, vl, user_class);
    if (user_obj == NULL)
      return (-1);
  }

  status = obj->cb_user_obj (ds, vl,
      user_class->user_class, user_obj->user_obj);
  if (status != 0)
  {
    ERROR ("utils_vl_lookup: The user object callback failed with status %i.",
        status);
    /* Returning a negative value means: abort! */
    if (status < 0)
      return (status);
    else
      return (1);
  }

  return (0);
} /* }}} int lu_handle_user_class */

static int lu_handle_user_class_list (lookup_t *obj, /* {{{ */
    data_set_t const *ds, value_list_t const *vl,
    user_class_list_t *user_class_list)
{
  user_class_list_t *ptr;
  int retval = 0;
  
  for (ptr = user_class_list; ptr != NULL; ptr = ptr->next)
  {
    int status;

    status = lu_handle_user_class (obj, ds, vl, &ptr->entry);
    if (status < 0)
      return (status);
    else if (status == 0)
      retval++;
  }

  return (retval);
} /* }}} int lu_handle_user_class_list */

static by_type_entry_t *lu_search_by_type (lookup_t *obj, /* {{{ */
    char const *type, _Bool allocate_if_missing)
{
  by_type_entry_t *by_type;
  char *type_copy;
  int status;

  status = c_avl_get (obj->by_type_tree, type, (void *) &by_type);
  if (status == 0)
    return (by_type);

  if (!allocate_if_missing)
    return (NULL);

  type_copy = strdup (type);
  if (type_copy == NULL)
  {
    ERROR ("utils_vl_lookup: strdup failed.");
    return (NULL);
  }

  by_type = malloc (sizeof (*by_type));
  if (by_type == NULL)
  {
    ERROR ("utils_vl_lookup: malloc failed.");
    sfree (type_copy);
    return (NULL);
  }
  memset (by_type, 0, sizeof (*by_type));
  by_type->wildcard_plugin_list = NULL;
  
  by_type->by_plugin_tree = c_avl_create ((void *) strcmp);
  if (by_type->by_plugin_tree == NULL)
  {
    ERROR ("utils_vl_lookup: c_avl_create failed.");
    sfree (by_type);
    sfree (type_copy);
    return (NULL);
  }

  status = c_avl_insert (obj->by_type_tree,
      /* key = */ type_copy, /* value = */ by_type);
  assert (status <= 0); /* >0 => entry exists => race condition. */
  if (status != 0)
  {
    ERROR ("utils_vl_lookup: c_avl_insert failed.");
    c_avl_destroy (by_type->by_plugin_tree);
    sfree (by_type);
    sfree (type_copy);
    return (NULL);
  }
  
  return (by_type);
} /* }}} by_type_entry_t *lu_search_by_type */

static int lu_add_by_plugin (by_type_entry_t *by_type, /* {{{ */
    identifier_t const *ident, user_class_list_t *user_class_list)
{
  user_class_list_t *ptr = NULL;

  /* Lookup user_class_list from the per-plugin structure. If this is the first
   * user_class to be added, the blocks return immediately. Otherwise they will
   * set "ptr" to non-NULL. */
  if (LU_IS_WILDCARD (ident->plugin))
  {
    if (by_type->wildcard_plugin_list == NULL)
    {
      by_type->wildcard_plugin_list = user_class_list;
      return (0);
    }

    ptr = by_type->wildcard_plugin_list;
  } /* if (plugin is wildcard) */
  else /* (plugin is not wildcard) */
  {
    int status;

    status = c_avl_get (by_type->by_plugin_tree,
        ident->plugin, (void *) &ptr);

    if (status != 0) /* plugin not yet in tree */
    {
      char *plugin_copy = strdup (ident->plugin);

      if (plugin_copy == NULL)
      {
        ERROR ("utils_vl_lookup: strdup failed.");
        sfree (user_class_list);
        return (ENOMEM);
      }

      status = c_avl_insert (by_type->by_plugin_tree,
          plugin_copy, user_class_list);
      if (status != 0)
      {
        ERROR ("utils_vl_lookup: c_avl_insert(\"%s\") failed with status %i.",
            plugin_copy, status);
        sfree (plugin_copy);
        sfree (user_class_list);
        return (status);
      }
      else
      {
        return (0);
      }
    } /* if (plugin not yet in tree) */
  } /* if (plugin is not wildcard) */

  assert (ptr != NULL);

  while (ptr->next != NULL)
    ptr = ptr->next;
  ptr->next = user_class_list;

  return (0);
} /* }}} int lu_add_by_plugin */

static void lu_destroy_user_obj (lookup_t *obj, /* {{{ */
    user_obj_t *user_obj)
{
  while (user_obj != NULL)
  {
    user_obj_t *next = user_obj->next;

    if (obj->cb_free_obj != NULL)
      obj->cb_free_obj (user_obj->user_obj);
    user_obj->user_obj = NULL;

    sfree (user_obj);
    user_obj = next;
  }
} /* }}} void lu_destroy_user_obj */

static void lu_destroy_user_class_list (lookup_t *obj, /* {{{ */
    user_class_list_t *user_class_list)
{
  while (user_class_list != NULL)
  {
    user_class_list_t *next = user_class_list->next;

    if (obj->cb_free_class != NULL)
      obj->cb_free_class (user_class_list->entry.user_class);
    user_class_list->entry.user_class = NULL;

    lu_destroy_user_obj (obj, user_class_list->entry.user_obj_list);
    user_class_list->entry.user_obj_list = NULL;

    sfree (user_class_list);
    user_class_list = next;
  }
} /* }}} void lu_destroy_user_class_list */

static void lu_destroy_by_type (lookup_t *obj, /* {{{ */
    by_type_entry_t *by_type)
{
  
  while (42)
  {
    char *plugin = NULL;
    user_class_list_t *user_class_list = NULL;
    int status;

    status = c_avl_pick (by_type->by_plugin_tree,
        (void *) &plugin, (void *) &user_class_list);
    if (status != 0)
      break;

    DEBUG ("utils_vl_lookup: lu_destroy_by_type: Destroying plugin \"%s\".",
        plugin);
    sfree (plugin);
    lu_destroy_user_class_list (obj, user_class_list);
  }

  c_avl_destroy (by_type->by_plugin_tree);
  by_type->by_plugin_tree = NULL;

  lu_destroy_user_class_list (obj, by_type->wildcard_plugin_list);
  by_type->wildcard_plugin_list = NULL;

  sfree (by_type);
} /* }}} int lu_destroy_by_type */

/*
 * Public functions
 */
lookup_t *lookup_create (lookup_class_callback_t cb_user_class, /* {{{ */
    lookup_obj_callback_t cb_user_obj,
    lookup_free_class_callback_t cb_free_class,
    lookup_free_obj_callback_t cb_free_obj)
{
  lookup_t *obj = malloc (sizeof (*obj));
  if (obj == NULL)
  {
    ERROR ("utils_vl_lookup: malloc failed.");
    return (NULL);
  }
  memset (obj, 0, sizeof (*obj));

  obj->by_type_tree = c_avl_create ((void *) strcmp);
  if (obj->by_type_tree == NULL)
  {
    ERROR ("utils_vl_lookup: c_avl_create failed.");
    sfree (obj);
    return (NULL);
  }

  obj->cb_user_class = cb_user_class;
  obj->cb_user_obj = cb_user_obj;
  obj->cb_free_class = cb_free_class;
  obj->cb_free_obj = cb_free_obj;

  return (obj);
} /* }}} lookup_t *lookup_create */

void lookup_destroy (lookup_t *obj) /* {{{ */
{
  int status;

  if (obj == NULL)
    return;

  while (42)
  {
    char *type = NULL;
    by_type_entry_t *by_type = NULL;

    status = c_avl_pick (obj->by_type_tree, (void *) &type, (void *) &by_type);
    if (status != 0)
      break;

    DEBUG ("utils_vl_lookup: lookup_destroy: Destroying type \"%s\".", type);
    sfree (type);
    lu_destroy_by_type (obj, by_type);
  }

  c_avl_destroy (obj->by_type_tree);
  obj->by_type_tree = NULL;

  sfree (obj);
} /* }}} void lookup_destroy */

int lookup_add (lookup_t *obj, /* {{{ */
    identifier_t const *ident, void *user_class)
{
  by_type_entry_t *by_type = NULL;
  user_class_list_t *user_class_obj;

  by_type = lu_search_by_type (obj, ident->type, /* allocate = */ 1);
  if (by_type == NULL)
    return (-1);

  user_class_obj = malloc (sizeof (*user_class_obj));
  if (user_class_obj == NULL)
  {
    ERROR ("utils_vl_lookup: malloc failed.");
    return (ENOMEM);
  }
  memset (user_class_obj, 0, sizeof (*user_class_obj));
  user_class_obj->entry.user_class = user_class;
  memmove (&user_class_obj->entry.ident, ident, sizeof (*ident));
  user_class_obj->entry.user_obj_list = NULL;
  user_class_obj->next = NULL;

  return (lu_add_by_plugin (by_type, ident, user_class_obj));
} /* }}} int lookup_add */

/* returns the number of successful calls to the callback function */
int lookup_search (lookup_t *obj, /* {{{ */
    data_set_t const *ds, value_list_t const *vl)
{
  by_type_entry_t *by_type = NULL;
  user_class_list_t *user_class_list = NULL;
  int retval = 0;
  int status;

  if ((obj == NULL) || (ds == NULL) || (vl == NULL))
    return (-EINVAL);

  by_type = lu_search_by_type (obj, vl->type, /* allocate = */ 0);
  if (by_type == NULL)
    return (0);

  status = c_avl_get (by_type->by_plugin_tree,
      vl->plugin, (void *) &user_class_list);
  if (status == 0)
  {
    status = lu_handle_user_class_list (obj, ds, vl, user_class_list);
    if (status < 0)
      return (status);
    retval += status;
  }

  if (by_type->wildcard_plugin_list != NULL)
  {
    status = lu_handle_user_class_list (obj, ds, vl,
        by_type->wildcard_plugin_list);
    if (status < 0)
      return (status);
    retval += status;
  }
    
  return (retval);
} /* }}} lookup_search */
