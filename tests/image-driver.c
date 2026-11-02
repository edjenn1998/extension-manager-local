/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glycin.h>

int main (int argc, char **argv)
{
    if (argc != 2)
        return 64;
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) bytes = NULL;
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents (argv[1], &contents, &length, &error))
        goto failure;
    bytes = g_bytes_new_take (contents, length);
    GlyLoader *loader = gly_loader_new_for_bytes (bytes);
    GlyImage *image = gly_loader_load (loader, &error);
    g_object_unref (loader);
    if (!image)
        goto failure;
    GlyFrame *frame = gly_image_next_frame (image, &error);
    g_object_unref (image);
    if (!frame)
        goto failure;
    g_print ("Decoded %u x %u: %s\n", gly_frame_get_width (frame), gly_frame_get_height (frame), argv[1]);
    g_object_unref (frame);
    return 0;
failure:
    g_printerr ("%s\n", error->message);
    return 1;
}
