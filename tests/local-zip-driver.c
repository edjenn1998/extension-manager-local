/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "local/exm-local-zip.h"
#include <stdio.h>

int main (int argc, char **argv)
{
    if (argc < 3)
        return 64;
    g_autoptr (GError) error = NULL;
    g_autoptr (ExmLocalZip) zip = exm_local_zip_inspect (argv[2], &error);
    if (!zip)
        goto failure;
    if (g_str_equal (argv[1], "install") || g_str_equal (argv[1], "replace"))
    {
        if (!exm_local_zip_install (zip, g_str_equal (argv[1], "replace"), &error))
            goto failure;
    }
    else if (g_str_equal (argv[1], "supports"))
        return argc == 4 && exm_local_zip_supports (zip, argv[3]) ? 0 : 3;
    else if (g_str_equal (argv[1], "snapshot"))
    {
        /* Replacing the original after validation must not affect install. */
        if (!g_file_set_contents (argv[2], "not a zip anymore", -1, &error) ||
            !exm_local_zip_install (zip, FALSE, &error))
            goto failure;
    }
    printf ("%s\n%s\n%s\n", zip->uuid, zip->name, zip->version ? zip->version : "no version");
    return 0;
failure:
    fprintf (stderr, "%s\n", error->message);
    return 1;
}
