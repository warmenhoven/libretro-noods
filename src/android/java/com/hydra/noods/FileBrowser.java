package com.hydra.noods;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Environment;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;

public class FileBrowser extends AppCompatActivity
{
    static
    {
        // Load the native library
        System.loadLibrary("noods-core");
    }

    private ArrayList<String> fileList;
    private ListView fileView;
    private String path;

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);

        // Request storage permissions
        if (ContextCompat.checkSelfPermission(this, android.Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_DENIED)
            ActivityCompat.requestPermissions(this, new String[] { android.Manifest.permission.READ_EXTERNAL_STORAGE }, 0);

        fileList = new ArrayList<String>();
        fileView = new ListView(this);
        path = Environment.getExternalStorageDirectory().getAbsolutePath();

        fileView.setOnItemClickListener(new AdapterView.OnItemClickListener()
        {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id)
            {
                // Navigate to the selected directory
                path += "/" + fileList.get(position);
                update();
            }
        });

        setContentView(fileView);
        update();

        loadSettings(path);
    }

    @Override
    public void onBackPressed()
    {
        // Navigate to the previous directory
        if (!path.equals(Environment.getExternalStorageDirectory().getAbsolutePath()))
        {
            path = path.substring(0, path.lastIndexOf('/'));
            update();
        }
    }

    private void update()
    {
        File file = new File(path);
        String ext = (file.getName().length() >= 4) ? file.getName().substring(file.getName().length() - 4) : "";

        // Check if the current file is a ROM
        if (!file.isDirectory() && (ext.equals(".nds") || ext.equals(".gba")))
        {
            // Attempt to load the ROM
            int result = loadRom(path);

            if (result == 0)
            {
                // Start the emulator if loading was successful
                startActivity(new Intent(this, NooActivity.class));
            }
            else
            {
                AlertDialog.Builder builder = new AlertDialog.Builder(this);

                // Inform the user of the error if loading was not successful
                switch (result)
                {
                    case 1: // Missing BIOS and/or firmware files
                    {
                        builder.setTitle("Missing BIOS/Firmware");
                        builder.setMessage("Initialization failed. " +
                                           "Make sure the path settings point to valid BIOS and firmware files and try again. " +
                                           "You can modify the path settings in the noods.ini file.");
                        break;
                    }

                    case 2: // Unreadable ROM file
                    {
                        builder.setTitle("Unreadable ROM");
                        builder.setMessage("Initialization failed. " +
                                           "Make sure the ROM file is accessible and try again.");
                        break;
                    }

                    case 3: // Missing save file
                    {
                        builder.setTitle("Missing Save");
                        builder.setMessage("This ROM does not have a save file. " +
                                           "NooDS cannot automatically detect save type. " +
                                           "Please provide a save with the correct file size.");
                        break;
                    }
                }

                builder.setPositiveButton("OK", new DialogInterface.OnClickListener()
                {
                    @Override
                    public void onClick(DialogInterface dialog, int id)
                    {
                        // Close the dialog
                    }
                });

                builder.create().show();
                onBackPressed();
            }

            return;
        }

        File[] files = file.listFiles();
        fileList.clear();

        // Get all the folders and ROMs at the current path
        for (int i = 0; i < files.length; i++)
        {
            ext = (files[i].getName().length() >= 4) ? files[i].getName().substring(files[i].getName().length() - 4) : "";
            if (files[i].isDirectory() || ext.equals(".nds") || ext.equals(".gba"))
                fileList.add(files[i].getName());
        }

        Collections.sort(fileList);
        fileView.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, fileList));
    }

    public native void loadSettings(String rootPath);
    public native int loadRom(String romPath);
}