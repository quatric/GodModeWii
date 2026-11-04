#ifndef SAVES_H
#define SAVES_H

/* HOME menu > "Export all saves": copies the save data folder of every
 * installed disc game, channel and WiiWare title off the NAND to
 * <SD or USB>:/GodModeWii/save-export/<date-time>/<title id> [<game id>]/data.
 * The NAND is only read. */
void saves_export_all(void);

#endif
